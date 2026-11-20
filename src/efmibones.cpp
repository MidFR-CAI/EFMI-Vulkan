#include "efmibones.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "common.h"
#include "log.h"

namespace efmivulkan::efmibones {
namespace {

// The ring: one segment per frame in flight, each free again when its frame comes round,
// plus room past the end for the config range the shaders declare.
constexpr uint32_t kSegments = 24;  // Some command buffers are submitted 3 frames after recording, then run later still.
constexpr VkDeviceSize kSegmentBytes = 2ull << 20;  // Peak seen in game: 612 KB a frame.
// The first kStaticRows rows: identity bones, as EFMI's implicit bones at the start of its
// merged buffer, where config offsets below the static rows point.
constexpr VkDeviceSize kStaticBytes = VkDeviceSize(kStaticRows) * 16;
constexpr VkDeviceSize kRingBytes = kStaticBytes + kSegments * kSegmentBytes + kConfigRange;
constexpr VkDeviceSize kAlign = 256;  // The most minUniformBufferOffsetAlignment can be.

DeviceFns g_fns;
Lookups g_lookups;

std::mutex g_ringMu;  // Creating the ring.
std::atomic<bool> g_ringTried{false};
VkBuffer g_ring = VK_NULL_HANDLE;
uint8_t* g_ringMapped = nullptr;
std::atomic<uint64_t> g_frame{0};
std::atomic<VkDeviceSize> g_cursor[kSegments]{};
std::atomic<uint64_t> g_ringFull{0};
std::atomic<uint64_t> g_zeroFills{0};  // Fills whose bones were all zero (nothing imported for them yet).
std::atomic<uint64_t> g_imports{0}, g_importsNoSkeleton{0}, g_fills{0}, g_ringPeak{0}, g_noConfig{0}, g_staleConfig{0};

bool CreateRing() {
    if (g_ring) return true;
    if (g_ringTried.exchange(true)) return g_ring != VK_NULL_HANDLE;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = kRingBytes;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBuffer buffer;
    if (g_fns.CreateBuffer(g_fns.device, &bi, nullptr, &buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    g_fns.GetBufferMemoryRequirements(g_fns.device, buffer, &req);
    // CPU-written each frame, GPU-read once: VRAM the CPU can write if there is some.
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int type = -1;
    for (const VkMemoryPropertyFlags want : {host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, host}) {
        for (uint32_t i = 0; i < g_fns.memory.memoryTypeCount && type < 0; ++i) {
            if ((req.memoryTypeBits & (1u << i)) && (g_fns.memory.memoryTypes[i].propertyFlags & want) == want) type = static_cast<int>(i);
        }
        if (type >= 0) break;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = static_cast<uint32_t>(type);
    VkDeviceMemory memory;
    void* mapped = nullptr;
    if (type < 0 || g_fns.AllocateMemory(g_fns.device, &ai, nullptr, &memory) != VK_SUCCESS ||
        g_fns.BindBufferMemory(g_fns.device, buffer, memory, 0) != VK_SUCCESS ||
        g_fns.MapMemory(g_fns.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        LogError("merged skeleton: no memory for the bone ring");
        return false;
    }
    g_ringMapped = static_cast<uint8_t*>(mapped);
    static const float kId[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    for (uint32_t b = 0; b < kStaticRows / 3; ++b) memcpy(g_ringMapped + b * 48, kId, 48);
    g_ring = buffer;
    LogInfo("merged skeleton: bone ring of %llu MB ready", static_cast<unsigned long long>(kRingBytes >> 20));
    return true;
}

// Byte offset in the ring, or UINT64_MAX when this frame's segment is full.
VkDeviceSize Allocate(VkDeviceSize bytes) {
    bytes = (bytes + kAlign - 1) / kAlign * kAlign;
    const uint32_t segment = g_frame.load(std::memory_order_relaxed) % kSegments;
    const VkDeviceSize at = g_cursor[segment].fetch_add(bytes, std::memory_order_relaxed);
    if (at + bytes > kSegmentBytes) {
        g_ringFull.fetch_add(1, std::memory_order_relaxed);
        return UINT64_MAX;
    }
    return kStaticBytes + segment * kSegmentBytes + at;
}

// Work for submit time, when the game has written its bones and configs.
struct Job {
    bool fill = false;  // Else an import.
    Skeleton* s = nullptr;
    // The game's instance config as it was when the draw was recorded: by submit its memory
    // can hold another frame's.
    std::array<uint8_t, kConfigBytes> config{};
    // Import.
    uint32_t component = 0;
    int lod = 0;
    uint64_t frame = 0;
    uint32_t slot = 0;  // The spatial-identification slot: the skeleton region.
    // The component's bones (12 floats each, this frame's and last frame's), read from the
    // game's rolling buffer when recorded, into the version of the skeleton of that frame.
    std::vector<float> current, previous;
    bool hasCurrent = false, hasPrevious = false;
    // Fill.
    const std::vector<uint16_t>* palette = nullptr;
    VkDeviceSize configAt = 0, currentAt = 0, previousAt = 0;
    const uint8_t* liveConfig = nullptr;  // The game's config memory itself (CycleMode bit 1).
    // Import read at submit (CycleMode bit 4), or a check at submit of one read at record:
    // the game's bone buffer and its config, live.
    bool deferRead = false, check = false;
    SkinCheck skin;  // Fill: the mod's vertices, to skin a sample of on the CPU.
    const uint8_t* liveBones = nullptr;
    uint64_t boneRows = 0;
    uint64_t recordHash = 0;  // Check: the bones as read at record.
};

std::atomic<int> g_mode{0};  // CycleMode.
// The game's bones as read: with impossible values when read at record, at submit; read at
// record and different at submit (the game wrote them in between).
std::atomic<uint64_t> g_badAtRecord{0}, g_badAtSubmit{0}, g_changedBySubmit{0}, g_checked{0}, g_badLogged{0}, g_badFills{0}, g_badConfigs{0};
std::atomic<uint64_t> g_skinChecks{0}, g_skinBad{0}, g_skinLogged{0};
std::atomic<uint64_t> g_bonesGuessWrong{0}, g_bonesGuessLogged{0};  // Draws whose shader reads bones elsewhere than the old guess.  // RunFill's sample of vertices skinned on the CPU.

std::atomic<bool> g_any{false};  // Any job ever: until then the hooks return at once.
std::mutex g_jobMu;
std::unordered_map<VkCommandBuffer, std::vector<Job>>* g_jobs = new std::unordered_map<VkCommandBuffer, std::vector<Job>>();
std::unordered_map<VkCommandBuffer, std::vector<VkCommandBuffer>>* g_children = new std::unordered_map<VkCommandBuffer, std::vector<VkCommandBuffer>>();

void Push(VkCommandBuffer cb, const Job& job) {
    std::lock_guard lock(g_jobMu);
    (*g_jobs)[cb].push_back(job);
    g_any.store(true, std::memory_order_relaxed);
}

uint32_t Row5(const uint8_t* config, int i) {
    uint32_t v;
    memcpy(&v, config + kBoneOffsetsRow * 16 + i * 4, 4);
    return v;
}

// An instance config as Endfield's skinning shaders read it: row 5 has this and last
// frame's bone rows, inside the bone buffer, then a count and 1.0f. Other passes bind other
// 64 KB uniform buffers too (seen in game: rows of 2.4 48.5 97 145 ...).
bool ConfigValid(const uint8_t* config, uint64_t boneRows) {
    uint32_t r5[4];
    memcpy(r5, config + kBoneOffsetsRow * 16, sizeof(r5));
    return r5[3] == 0x3f800000u && r5[0] >= kStaticRows && r5[0] < boneRows && r5[1] < boneRows;
}

const float kIdentity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

// MergedSkeleton_BoneDataImporter_RollingBuffer.hlsl, its read: the component's bones from
// the game's buffer at the instance config's row 5 offsets (each only when past the static
// rows), LoD remapped, scaled by the mod's mesh scale. NaN marks a bone left as it is.
void CaptureImport(Job& j, const uint8_t* bones, uint64_t boneRows) {
    const Skeleton& s = *j.s;
    const Component& c = s.components[j.component];
    const uint32_t current = Row5(j.config.data(), 0), previous = Row5(j.config.data(), 1);
    const bool lodded = j.lod > 0 && j.lod < static_cast<int>(kMaxLods);
    // The remap checked in game (ObserveLod: the mod's own, corrected where wrong, or learned),
    // else the mod's own as it ships.
    std::shared_ptr<const std::vector<uint16_t>> learned;
    if (lodded) {
        std::lock_guard lock(j.s->mu);
        const size_t at = size_t(j.component) * kMaxLods + j.lod;
        if (at < s.learnedRemap.size()) learned = s.learnedRemap[at];
    }
    const std::vector<uint16_t>* remap = learned ? learned.get() : lodded && !c.lodRemap[j.lod].empty() ? &c.lodRemap[j.lod] : nullptr;
    const float scale = s.meshScale;
    const uint32_t count = c.offset < s.bones ? std::min(c.count, s.bones - c.offset) : 0;
    j.hasCurrent = current >= kStaticRows;
    j.hasPrevious = previous >= kStaticRows;
    const auto read = [&](std::vector<float>& to, uint32_t start) {
        to.assign(size_t(count) * 12, NAN);
        for (uint32_t b = 0; b < count; ++b) {
            const uint32_t source = remap ? (b < remap->size() ? (*remap)[b] : b) : b;
            const uint64_t row = uint64_t(start) + 3 + 3ull * source;
            if (row + 3 > boneRows) continue;
            float* dst = &to[size_t(b) * 12];
            memcpy(dst, bones + row * 16, 48);
            if (scale != 1.0f) {
                for (int k = 0; k < 12; ++k) dst[k] *= scale;
            }
        }
    };
    if (j.hasCurrent) read(j.current, current);
    if (j.hasPrevious) read(j.previous, previous);
}

// Bone values no real pose has: not a number, or past 10 km (bones are in the object's own
// space). NaN first is CaptureImport's mark for a bone not read.
bool BadBones(const std::vector<float>& v) {
    for (size_t b = 0; b * 12 < v.size(); ++b) {
        const float* m = &v[b * 12];
        if (std::isnan(m[0])) continue;
        for (int k = 0; k < 12; ++k) {
            if (!std::isfinite(m[k]) || std::fabs(m[k]) > 1e4f) return true;
        }
    }
    return false;
}

uint64_t HashBones(const std::vector<float>& v) {
    uint64_t h = 0xcbf29ce484222325ull;
    const auto* p = reinterpret_cast<const uint8_t*>(v.data());
    for (size_t i = 0; i < v.size() * sizeof(float); ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

void LogBad(const Job& j, const char* when) {
    if (g_badLogged.fetch_add(1, std::memory_order_relaxed) >= 30) return;
    LogInfo("merged skeleton: %s component %u slot %u: the game's bones read %s have impossible values", j.s->name.c_str(), j.component, j.slot, when);
}

// The newest version of a frame before `frame`, or null.
const SkeletonVersion* NewestBefore(const SkeletonInstance& in, uint64_t frame) {
    const SkeletonVersion* best = nullptr;
    for (const SkeletonVersion& v : in.versions) {
        if (v.frame != UINT64_MAX && v.frame < frame && (!best || v.frame > best->frame)) best = &v;
    }
    return best;
}

// The version of `frame`, made from the newest one before it the first time (bones not
// imported this frame stay as last imported). s.mu held.
SkeletonVersion& VersionOf(SkeletonInstance& in, uint64_t frame) {
    SkeletonVersion& v = in.versions[frame % kVersions];
    if (v.frame == frame) return v;
    if (v.frame != UINT64_MAX && v.frame > frame) return v;  // Recorded very late: the slot holds a newer frame.
    const SkeletonVersion* from = NewestBefore(in, frame);
    if (from && from != &v) {
        v.current = from->current;
        v.previous = from->previous;
    } else if (!from) {
        v.current.assign(in.floats, 0.0f);
        v.previous.assign(in.floats, 0.0f);
    }
    v.imported.assign(in.updateFrame.size(), 0);
    v.frame = frame;
    return v;
}

// Its write: into the slot's region, in the version of the frame it was recorded in, and in
// the versions of later frames already made from it that have not imported this component.
void RunImport(const Job& j) {
    g_imports.fetch_add(1, std::memory_order_relaxed);
    Skeleton& s = *j.s;
    if (j.component >= s.components.size() || j.slot >= kMaxInstances) return;
    const Component& c = s.components[j.component];
    std::lock_guard lock(s.mu);
    SkeletonInstance& in = s.instances[j.slot];
    const auto apply = [&](std::vector<float>& to, const std::vector<float>& from) {
        for (size_t b = 0; b * 12 < from.size(); ++b) {
            if (std::isnan(from[b * 12])) continue;
            const size_t at = (size_t(c.offset) + b) * 12;
            if (at + 12 <= to.size()) memcpy(&to[at], &from[b * 12], 48);
        }
    };
    const auto write = [&](SkeletonVersion& v) {
        if (j.hasCurrent) apply(v.current, j.current);
        if (j.hasPrevious) apply(v.previous, j.previous);
    };
    SkeletonVersion& mine = VersionOf(in, j.frame);
    write(mine);
    if (j.component < mine.imported.size()) mine.imported[j.component] = 1;
    for (SkeletonVersion& v : in.versions) {
        const bool own = j.component < v.imported.size() && v.imported[j.component];  // That frame's own import stands.
        if (&v != &mine && v.frame != UINT64_MAX && v.frame > j.frame && !own) write(v);
    }
}

// MergedSkeleton_InstanceConfigOverrider_RollingBuffer.hlsl: the instance's config with row 5
// pointing at the merged bones (each offset only when it was past the static rows), and the
// slot's bones this mod draw's palette uses (the 8-bit indices Vulkan's pipelines read).
void RunFill(const Job& j) {
    g_fills.fetch_add(1, std::memory_order_relaxed);
    const int mode = g_mode.load(std::memory_order_relaxed);
    const uint8_t* source = (mode & 1) && j.liveConfig ? j.liveConfig : j.config.data();
    uint8_t* config = g_ringMapped + j.configAt;
    memcpy(config, source, kConfigBytes);
    {
        // The object's transform (rows 0 to 3): impossible values throw the whole draw away.
        float t[16];
        memcpy(t, source, sizeof(t));
        bool bad = false;
        for (float v : t) bad = bad || !std::isfinite(v) || std::fabs(v) > 1e6f;
        if (bad) {
            g_badConfigs.fetch_add(1, std::memory_order_relaxed);
            if (g_badLogged.fetch_add(1, std::memory_order_relaxed) < 30) {
                LogInfo("merged skeleton: %s component %u slot %u: a mod draw got a config with an impossible transform", j.s->name.c_str(), j.component, j.slot);
            }
        }
    }
    uint32_t rows[2] = {Row5(source, 0), Row5(source, 1)};
    if (rows[0] >= kStaticRows) rows[0] = static_cast<uint32_t>(j.currentAt / 16);
    if (rows[1] >= kStaticRows) rows[1] = static_cast<uint32_t>(j.previousAt / 16);
    memcpy(config + kBoneOffsetsRow * 16, rows, sizeof(rows));
    Skeleton& s = *j.s;
    if (j.slot >= kMaxInstances) return;
    std::lock_guard lock(s.mu);
    SkeletonInstance& in = s.instances[j.slot];
    // The skeleton as of the frame this draw was recorded in. Gone (recorded more than
    // kVersions frames ago): the newest there is.
    const SkeletonVersion* version = nullptr;
    for (const SkeletonVersion& v : in.versions) {
        if (v.frame == j.frame && !(mode & 2)) version = &v;  // Mode 2 and 3: the newest, below.
    }
    if (!version && !(mode & 2)) version = NewestBefore(in, j.frame);
    if (!version) version = NewestBefore(in, UINT64_MAX);
    static const std::vector<float> kNone;
    for (int which = 0; which < 2; ++which) {
        uint8_t* block = g_ringMapped + (which ? j.previousAt : j.currentAt);
        const std::vector<float>& from = !version ? kNone : which ? version->previous : version->current;
        memcpy(block, kIdentity, 48);  // The 3 rows before bone 0 (the implicit bones' in EFMI).
        bool any = false;
        for (size_t i = 0; i < j.palette->size(); ++i) {
            const uint16_t m = (*j.palette)[i];
            if (size_t(m) * 12 + 12 <= from.size()) {
                memcpy(block + (3 + 3 * i) * 16, &from[size_t(m) * 12], 48);
                any = any || from[size_t(m) * 12] != 0.0f || from[size_t(m) * 12 + 5] != 0.0f;
            } else {
                memset(block + (3 + 3 * i) * 16, 0, 48);
            }
        }
        if (which == 0 && !any) g_zeroFills.fetch_add(1, std::memory_order_relaxed);
    }
    // A sample of the mod's vertices skinned as the vertex shader does, with the bones this
    // draw gets: a vertex far from the object is a triangle thrown across the screen.
    // One fill in 1024, 16 vertices: the mod's buffers are in VRAM, slow for the CPU to read.
    const SkinCheck& k = j.skin;
    const bool sampled = (g_fills.load(std::memory_order_relaxed) & 1023) == 0;
    if (sampled && version && k.positions && k.blend && k.vertices && (k.blendStride == 12 || k.blendStride == 32)) {
        g_skinChecks.fetch_add(1, std::memory_order_relaxed);
        const bool floatWeights = k.blendStride == 32;
        // The palette's bones from the CPU copy of the skeleton, as the fill put them in the ring.
        std::vector<float> paletteBones(j.palette->size() * 12, 0.0f);
        for (size_t i = 0; i < j.palette->size(); ++i) {
            const size_t m = (*j.palette)[i];
            if (m * 12 + 12 <= version->current.size()) memcpy(&paletteBones[i * 12], &version->current[m * 12], 48);
        }
        const float* bonesAt = paletteBones.data();
        const uint64_t step = std::max<uint64_t>(1, k.vertices / 16);
        for (uint64_t v = 0; v < k.vertices; v += step) {
            float p[3];
            memcpy(p, k.positions + v * k.positionStride, sizeof(p));
            const uint8_t* bl = k.blend + v * k.blendStride;
            float out[3] = {}, total = 0;
            uint32_t idx[4]{};
            float w[4]{};
            for (int n = 0; n < 4; ++n) {
                if (floatWeights) {
                    memcpy(&w[n], bl + 4 * n, 4);
                    memcpy(&idx[n], bl + 16 + 4 * n, 4);
                } else {
                    uint16_t u;
                    memcpy(&u, bl + 2 * n, 2);
                    w[n] = u / 65535.0f;
                    idx[n] = bl[8 + n];
                }
                if (w[n] == 0) continue;
                total += w[n];
                if (idx[n] >= j.palette->size()) {
                    out[0] = 1e9f;  // Past the palette: the GPU reads whatever follows the block.
                    break;
                }
                const float* m = bonesAt + size_t(idx[n]) * 12;
                for (int r = 0; r < 3; ++r) out[r] += w[n] * (m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3]);
            }
            const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
            if (len <= 5.0f || total == 0) continue;
            g_skinBad.fetch_add(1, std::memory_order_relaxed);
            if (g_skinLogged.fetch_add(1, std::memory_order_relaxed) < 40) {
                LogInfo("skin check: %s component %u slot %u: vertex %llu at %.2f %.2f %.2f lands %.1f m out; bones %u %u %u %u (palette of %zu), "
                        "weights %.2f %.2f %.2f %.2f",
                        s.name.c_str(), j.component, j.slot, (unsigned long long)v, p[0], p[1], p[2], len, idx[0], idx[1], idx[2], idx[3],
                        j.palette->size(), w[0], w[1], w[2], w[3]);
            }
            break;  // One a draw is enough.
        }
    }
    if (!j.palette->empty() && !s.fillLogged) {
        s.fillLogged = true;
        float first;
        memcpy(&first, g_ringMapped + j.currentAt + 3 * 16, 4);
        LogInfo("merged skeleton: first fill: bones at rows %u and %u of the ring, bone 0 (merged %u) starts %g", rows[0], rows[1],
                (*j.palette)[0], first);
    }
}

void Collect(VkCommandBuffer cb, std::vector<Job>& out, int depth) {
    const auto it = g_jobs->find(cb);
    if (it != g_jobs->end()) {
        out.insert(out.end(), it->second.begin(), it->second.end());
        g_jobs->erase(it);
    }
    if (depth > 0) return;
    const auto children = g_children->find(cb);
    if (children == g_children->end()) return;
    for (VkCommandBuffer child : children->second) Collect(child, out, depth + 1);
    g_children->erase(children);
}

}  // namespace

void Skeleton::Init(uint32_t boneCount, std::vector<Component> c) {
    bones = boneCount;
    components = std::move(c);
    boneComponent.assign(bones, 0xffff);
    for (size_t k = 0; k < components.size(); ++k) {
        for (uint32_t b = 0; b < components[k].count && components[k].offset + b < bones; ++b) boneComponent[components[k].offset + b] = static_cast<uint16_t>(k);
    }
    for (SkeletonInstance& in : instances) {
        in.floats = size_t(bones) * 12;
        for (SkeletonVersion& v : in.versions) {
            v.frame = UINT64_MAX;
            v.current.clear();
            v.previous.clear();
        }
        in.updateFrame.assign(components.size(), UINT64_MAX);
        in.lodLevel.assign(components.size(), 0);
    }
    learnedRemap.assign(components.size() * kMaxLods, nullptr);
    lodSamples.assign(components.size() * kMaxLods, LodSample{});
    lodLearning.assign(components.size() * kMaxLods, LodLearning{});
}

void SetDevice(const DeviceFns& fns) { g_fns = fns; }
void SetLookups(const Lookups& l) { g_lookups = l; }

bool FindBoneBinding(const efmitex::BoundSet* sets, BoneBinding* out, const std::vector<spirv::Resource>* vs) {
    if (!g_lookups.Contents || !g_lookups.Mapped) return false;
    efmitex::SetContents c;
    bool bones = false;
    VkDeviceSize bonesSize = 0;
    // Only the buffers the vertex shader declares, when its SPIR-V says which.
    const bool known = vs && !vs->empty();
    const auto declared = [&](uint32_t set, uint32_t binding, spirv::Kind kind) {
        if (!known) return true;
        for (const spirv::Resource& r : *vs) {
            if (r.set == set && r.binding == binding && r.kind == kind) return true;
        }
        return false;
    };
    // The old guess (the largest storage buffer at offset 0), to log where it was wrong.
    uint32_t guessSet = UINT32_MAX, guessBinding = 0;
    VkDeviceSize guessSize = 0;
    struct Candidate {
        uint32_t set, binding, element;
        const uint8_t* config;
        uint64_t available;
    };
    Candidate configs[8];
    uint32_t configCount = 0;
    for (uint32_t i = 0; i < efmitex::kMaxSets; ++i) {
        if (!sets[i].set || !g_lookups.Contents(sets[i].set, &c)) continue;
        for (const auto& e : c.entries) {
            if (!efmitex::IsBuffer(e.descriptorType)) continue;
            const bool uniform = e.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || e.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            for (uint32_t k = 0; k < e.descriptorCount; ++k) {
                const size_t at = e.offset + size_t(k) * e.stride;
                if (at + sizeof(VkDescriptorBufferInfo) > c.data.size()) break;
                VkDescriptorBufferInfo info;
                memcpy(&info, c.data.data() + at, sizeof(info));
                VkDeviceSize size = 0;
                const uint8_t* mapped = info.buffer ? g_lookups.Mapped(info.buffer, &size) : nullptr;
                if (!mapped) continue;
                VkDeviceSize offset = info.offset;
                if (efmitex::IsDynamic(e.descriptorType)) {
                    const int d = efmitex::DynamicIndex(c.entries, e.dstBinding, e.dstArrayElement + k);
                    if (d >= 0 && d < static_cast<int>(sets[i].dynamicCount)) offset += sets[i].dynamic[d];
                }
                const VkDeviceSize range = info.range == VK_WHOLE_SIZE ? size - offset : info.range;
                if (!uniform && offset == 0 && size > guessSize) {
                    guessSize = size;
                    guessSet = i;
                    guessBinding = e.dstBinding;
                }
                if (uniform && range == kConfigRange && offset + kConfigBytes <= size) {
                    if (configCount < 8 && declared(i, e.dstBinding, spirv::Kind::kUniformBuffer)) {
                        configs[configCount++] = {i, e.dstBinding, e.dstArrayElement + k, mapped + offset, std::min<uint64_t>(size - offset, kConfigRange)};
                    }
                } else if (!uniform && (known ? declared(i, e.dstBinding, spirv::Kind::kStorageBuffer) && range > bonesSize
                                              : offset == 0 && size > bonesSize)) {
                    // The shader's own storage buffer (at its offset), or the old guess.
                    bones = true;
                    bonesSize = known ? range : size;
                    out->bonesSet = i;
                    out->bonesBinding = e.dstBinding;
                    out->bonesElement = e.dstArrayElement + k;
                    out->bones = mapped + offset;
                    out->boneRows = (known ? range : size) / 16;
                }
            }
        }
    }
    if (!bones) return false;
    if (known && (guessSet != out->bonesSet || guessBinding != out->bonesBinding)) {
        g_bonesGuessWrong.fetch_add(1, std::memory_order_relaxed);
        if (g_bonesGuessLogged.fetch_add(1, std::memory_order_relaxed) < 20) {
            LogInfo("merged skeleton: the vertex shader reads its bones at set %u binding %u; the old guess was set %u binding %u", out->bonesSet,
                    out->bonesBinding, guessSet, guessBinding);
        }
    }
    for (uint32_t k = 0; k < configCount; ++k) {
        if (!ConfigValid(configs[k].config, out->boneRows)) continue;
        out->configSet = configs[k].set;
        out->configBinding = configs[k].binding;
        out->configElement = configs[k].element;
        out->config = configs[k].config;
        out->configAvailable = configs[k].available;
        return true;
    }
    g_noConfig.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool UsualBones(const BoneBinding& b, uint32_t sampleBones, std::string* why) {
    if (!b.config) return false;
    float t[16];
    memcpy(t, b.config, sizeof(t));
    char text[512];
    const auto fail = [&](const char* reason, float a = 0, float c = 0, float d = 0) {
        if (why) {
            snprintf(text, sizeof(text), "%s (%g %g %g); rows 0-3: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g; row 5: %u %u", reason, a, c, d, t[0],
                     t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], t[12], t[13], t[14], t[15], Row5(b.config, 0), Row5(b.config, 1));
            *why = text;
        }
        return false;
    };
    if (!std::isfinite(t[15]) || std::fabs(t[15] - 1.0f) > 1e-3f) return fail("row 3 w is not 1", t[15]);  // Row 3: the position, w 1.
    for (int r = 0; r < 4; ++r) {
        for (int k = 0; k < 4; ++k) {
            if (!std::isfinite(t[r * 4 + k]) || std::fabs(t[r * 4 + k]) > 1e6f) return fail("transform out of range", float(r), float(k), t[r * 4 + k]);
        }
    }
    for (int r = 0; r < 3; ++r) {
        const float len = std::sqrt(t[r * 4] * t[r * 4] + t[r * 4 + 1] * t[r * 4 + 1] + t[r * 4 + 2] * t[r * 4 + 2]);
        if (!(len > 1e-3f && len < 1e3f)) return fail("rotation row length", float(r), len);  // Rotation and scale.
    }
    const uint32_t first = Row5(b.config, 0);
    if (first < kStaticRows || !b.bones) return true;  // No skeleton of its own: nothing more to tell.
    for (uint32_t i = 0; i < sampleBones; ++i) {
        const uint64_t row = uint64_t(first) + 3 + 3ull * i;
        if (row + 3 > b.boneRows) break;
        float m[12];
        memcpy(m, b.bones + row * 16, sizeof(m));
        for (float v : m) {
            if (!std::isfinite(v)) return fail("bone not a number", float(i));
        }
        if (std::fabs(m[3]) > 50.0f || std::fabs(m[7]) > 50.0f || std::fabs(m[11]) > 50.0f) return fail("bone translation past 50 m", m[3], m[7], m[11]);
    }
    return true;
}

namespace {

// The save file of learned remaps: one line each, "<mod>|<component>|<lod> = <bone> <bone> ...".
void SaveLearnedRemap(const std::string& mod, uint32_t component, int lod, const std::vector<uint16_t>& remap) {
    std::string line = mod + "|" + std::to_string(component) + "|" + std::to_string(lod) + " =";
    for (const uint16_t r : remap) line += " " + std::to_string(r);
    line += "\r\n";
    PostJob([line] {
        FILE* f = nullptr;
        _wfopen_s(&f, (LogDir() + L"\\lodremaps.ini").c_str(), L"ab");
        if (!f) return;
        fwrite(line.data(), 1, line.size(), f);
        fclose(f);
    });
}

// One frame's evidence: each LoD 0 bone's possible places in LoD n's list (the same matrix:
// both are this frame's pose of the same bone). Many bones share a matrix in a frame (those at
// rest), so one frame cannot tell them apart: the candidates are narrowed over frames as the
// object moves (Skeleton::LodLearning). False when too few bones match to trust the pairing.
bool NarrowCandidates(const std::vector<float>& lod0, const std::vector<float>& lodN, Skeleton::LodLearning& l) {
    const size_t n0 = lod0.size() / 12, nN = lodN.size() / 12;
    std::vector<std::vector<uint16_t>> now(n0);
    size_t matched = 0;
    for (size_t i = 0; i < n0; ++i) {
        const float* a = &lod0[i * 12];
        for (size_t k = 0; k < nN; ++k) {
            const float* b = &lodN[k * 12];
            float worst = 0;
            for (int e = 0; e < 12 && worst < 1e-4f; ++e) worst = std::max(worst, std::fabs(a[e] - b[e]));
            if (worst < 1e-4f) now[i].push_back(static_cast<uint16_t>(k));
        }
        matched += !now[i].empty();
    }
    if (matched < std::max<size_t>(1, n0 / 2)) return false;
    if (l.candidates.size() != n0) l.candidates.assign(n0, {});
    for (size_t i = 0; i < n0; ++i) {
        if (now[i].empty()) continue;  // Not in LoD n this frame (a bone it lacks), or a rounding miss.
        std::vector<uint16_t>& c = l.candidates[i];
        if (c.empty()) {
            c = now[i];
            continue;
        }
        std::vector<uint16_t> both;
        for (uint16_t k : c) {
            if (std::find(now[i].begin(), now[i].end(), k) != now[i].end()) both.push_back(k);
        }
        c = both.empty() ? now[i] : both;  // None in common: the earlier frames were a coincidence.
    }
    ++l.frames;
    l.last = lodN;
    l.lastZero = lod0;
    return true;
}

// The remap, once every LoD 0 bone has one place (or never moved apart from others in many
// frames: the lowest of them). A bone LoD n lacks takes LoD n's bone nearest to it.
std::vector<uint16_t> Settle(const Skeleton::LodLearning& l) {
    constexpr uint32_t kMinFrames = 30, kMaxFrames = 600;
    if (l.frames < kMinFrames) return {};
    size_t lastPlace = 0;
    bool open = false;
    for (const auto& c : l.candidates) {
        if (c.size() > 1) open = true;
        for (uint16_t k : c) lastPlace = std::max<size_t>(lastPlace, k);
    }
    if (open && l.frames < kMaxFrames) return {};
    std::vector<uint16_t> remap(l.candidates.size(), 0);
    const size_t nN = l.last.size() / 12;
    for (size_t i = 0; i < l.candidates.size(); ++i) {
        if (!l.candidates[i].empty()) {
            remap[i] = *std::min_element(l.candidates[i].begin(), l.candidates[i].end());
            continue;
        }
        const float* a = &l.lastZero[i * 12];
        float best = FLT_MAX;
        for (size_t k = 0; k <= lastPlace && k < nN; ++k) {
            const float* b = &l.last[k * 12];
            const float dx = a[3] - b[3], dy = a[7] - b[7], dz = a[11] - b[11];
            if (dx * dx + dy * dy + dz * dz < best) {
                best = dx * dx + dy * dy + dz * dz;
                remap[i] = static_cast<uint16_t>(k);
            }
        }
    }
    return remap;
}

}  // namespace

void ObserveLod(Skeleton* s, uint32_t component, int lod, const BoneBinding& b) {
    if (component >= s->components.size() || lod < 0 || lod >= static_cast<int>(kMaxLods) || !b.config || !b.bones) return;
    const Component& c = s->components[component];
    // Learned for every LoD, the mod's own remap or not: one the mod ships may be for LoD
    // meshes of another game version (never used, if the mod had no LoD entries for them).
    const auto needs = [&](int l) {
        const size_t at = size_t(component) * kMaxLods + l;
        return l > 0 && at < s->learnedRemap.size() && !s->learnedRemap[at];
    };
    {
        std::lock_guard lock(s->mu);
        bool any = false;
        for (int l = 1; l < static_cast<int>(kMaxLods); ++l) any = any || needs(l);
        if (!any || (lod > 0 && !needs(lod))) return;
    }
    const uint32_t first = Row5(b.config, 0);
    if (first < kStaticRows || !c.count) return;
    Skeleton::LodSample sample;
    sample.frame = g_frame.load(std::memory_order_relaxed);
    memcpy(sample.position, b.config + 3 * 16, sizeof(sample.position));
    sample.bones.assign(size_t(c.count) * 12, 0.0f);
    for (uint32_t i = 0; i < c.count; ++i) {
        const uint64_t row = uint64_t(first) + 3 + 3ull * i;
        if (row + 3 > b.boneRows) {
            sample.bones.resize(size_t(i) * 12);
            break;
        }
        memcpy(&sample.bones[size_t(i) * 12], b.bones + row * 16, 48);
    }
    std::lock_guard lock(s->mu);
    const size_t base = size_t(component) * kMaxLods;
    if (base + kMaxLods > s->lodSamples.size()) return;
    s->lodSamples[base + lod] = std::move(sample);
    const Skeleton::LodSample& zero = s->lodSamples[base];
    for (int l = 1; l < static_cast<int>(kMaxLods); ++l) {
        const Skeleton::LodSample& other = s->lodSamples[base + l];
        if (!needs(l) || zero.frame != other.frame || zero.frame == UINT64_MAX) continue;
        const float dx = zero.position[0] - other.position[0], dy = zero.position[1] - other.position[1], dz = zero.position[2] - other.position[2];
        if (dx * dx + dy * dy + dz * dz > 0.25f) continue;  // Two objects: not one draw's LoDs.
        Skeleton::LodLearning& learning = s->lodLearning[base + l];
        if (learning.lastFrame == zero.frame || !NarrowCandidates(zero.bones, other.bones, learning)) continue;
        learning.lastFrame = zero.frame;
        std::vector<uint16_t> remap = Settle(learning);
        if (remap.empty()) continue;
        // The mod's own remap stands for every bone the frames do not prove wrong (its bone
        // never matched): where bones stayed alike, its choice is as good as any.
        const std::vector<uint16_t>& own = c.lodRemap[l];
        size_t differ = 0;
        for (size_t i = 0; i < remap.size() && i < own.size(); ++i) {
            const std::vector<uint16_t>& cand = learning.candidates[i];
            if (cand.empty() || std::find(cand.begin(), cand.end(), own[i]) != cand.end()) {
                remap[i] = own[i];
            } else {
                ++differ;
            }
        }
        if (own.empty()) {
            LogInfo("merged skeleton: %s component %u: LoD %d's bone order learned in game over %u frames (%zu bones; the mod has no remap for it)",
                    s->name.c_str(), component, l, learning.frames, remap.size());
        } else {
            LogInfo("merged skeleton: %s component %u: LoD %d's bone order checked in game over %u frames; the mod's own remap is wrong for %zu of %zu "
                    "bones, corrected",
                    s->name.c_str(), component, l, learning.frames, differ, remap.size());
        }
        SaveLearnedRemap(s->name, component, l, remap);
        s->learnedRemap[base + l] = std::make_shared<const std::vector<uint16_t>>(std::move(remap));
    }
}

bool HasLodRemap(Skeleton* s, uint32_t component, int lod) {
    if (lod <= 0) return true;
    if (component >= s->components.size() || lod >= static_cast<int>(kMaxLods)) return false;
    if (!s->components[component].lodRemap[lod].empty()) return true;
    std::lock_guard lock(s->mu);
    const size_t at = size_t(component) * kMaxLods + lod;
    return at < s->learnedRemap.size() && s->learnedRemap[at] != nullptr;
}

void SetLearnedRemap(Skeleton* s, uint32_t component, int lod, std::vector<uint16_t> remap) {
    std::lock_guard lock(s->mu);
    const size_t at = size_t(component) * kMaxLods + lod;
    if (at < s->learnedRemap.size() && lod > 0) s->learnedRemap[at] = std::make_shared<const std::vector<uint16_t>>(std::move(remap));
}

void CopyInstance(Skeleton* s, uint32_t from, uint32_t to) {
    if (from >= kMaxInstances || to >= kMaxInstances || from == to) return;
    std::lock_guard lock(s->mu);
    s->instances[to] = s->instances[from];
    // Imported again for this slot from its own draws, this frame already.
    std::fill(s->instances[to].updateFrame.begin(), s->instances[to].updateFrame.end(), UINT64_MAX);
}

void Import(VkCommandBuffer cb, Skeleton* s, uint32_t component, int lod, const BoneBinding& b, uint32_t slot, const uint8_t* config) {
    if (component >= s->components.size() || slot >= kMaxInstances) return;
    const uint64_t frame = g_frame.load(std::memory_order_relaxed);
    {
        // CommandListMergedSkeleton_Apply: once a frame per component and slot, again for a
        // better (lower) LoD.
        std::lock_guard lock(s->mu);
        SkeletonInstance& in = s->instances[slot];
        if (in.updateFrame[component] == frame && lod >= in.lodLevel[component]) return;
        in.updateFrame[component] = frame;
        in.lodLevel[component] = lod;
    }
    Job j;
    j.s = s;
    memcpy(j.config.data(), config, kConfigBytes);
    j.component = component;
    j.lod = lod;
    j.frame = frame;
    j.slot = slot;
    j.liveBones = b.bones;
    j.boneRows = b.boneRows;
    j.liveConfig = config;
    if (g_mode.load(std::memory_order_relaxed) & 4) {
        // Read at submit (CycleMode bit 4): in recorded order with the fills, from the game's
        // memory as it is then.
        j.deferRead = true;
        Push(cb, j);
        return;
    }
    // Read now, as the draw is recorded, into this frame's version of the skeleton.
    CaptureImport(j, b.bones, b.boneRows);
    if (BadBones(j.current)) {
        g_badAtRecord.fetch_add(1, std::memory_order_relaxed);
        LogBad(j, "as the draw was recorded");
    }
    RunImport(j);
    // (A check at submit of whether the game wrote them after the draw was recorded found it
    // does not: 2 in 50,000. Gone, as it read the game's memory a second time per import.)
}

bool Redirect(VkCommandBuffer cb, Skeleton* s, uint32_t component, int lod, const std::vector<uint16_t>* palette, const BoneBinding& b,
              uint32_t slot, const uint8_t* config, efmitex::BufferPatch patches[2], const SkinCheck* check) {
    if (!CreateRing()) return false;
    const VkDeviceSize blockBytes = (3 + 3 * VkDeviceSize(palette->size())) * 16;
    const VkDeviceSize configAt = Allocate(kConfigBytes), currentAt = Allocate(blockBytes), previousAt = Allocate(blockBytes);
    if (configAt == UINT64_MAX || currentAt == UINT64_MAX || previousAt == UINT64_MAX) return false;
    Job j;
    j.fill = true;
    j.s = s;
    j.component = component;
    j.lod = lod;
    memcpy(j.config.data(), config, kConfigBytes);
    j.liveConfig = config;
    if (check) j.skin = *check;
    j.palette = palette;
    j.configAt = configAt;
    j.currentAt = currentAt;
    j.previousAt = previousAt;
    j.frame = g_frame.load(std::memory_order_relaxed);
    j.slot = slot;
    Push(cb, j);
    // The config: the ring at offset 0 with the block as dynamic offset, or as offset when
    // the descriptor is not dynamic (efmitex::Bind uses whichever the descriptor takes).
    patches[0] = {b.configSet, b.configBinding, b.configElement, g_ring, 0, kConfigRange, static_cast<uint32_t>(configAt)};
    patches[1] = {b.bonesSet, b.bonesBinding, b.bonesElement, g_ring, 0, kRingBytes, 0};
    return true;
}

void OnBegin(VkCommandBuffer cb) {
    if (!g_any.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_jobMu);
    g_jobs->erase(cb);
    g_children->erase(cb);
}

void OnExecute(VkCommandBuffer primary, uint32_t count, const VkCommandBuffer* secondaries) {
    if (!g_any.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_jobMu);
    auto& children = (*g_children)[primary];
    children.insert(children.end(), secondaries, secondaries + count);
}

void OnSubmitted(const VkCommandBuffer* cbs, uint32_t count) {
    if (!g_any.load(std::memory_order_relaxed)) return;
    std::vector<Job> jobs;
    {
        std::lock_guard lock(g_jobMu);
        for (uint32_t i = 0; i < count; ++i) Collect(cbs[i], jobs, 0);
    }
    // In recorded order: a mod draw gets the imports recorded before it, as EFMI's.
    for (const Job& j : jobs) {
        if (j.fill) {
            Guard("bones fill", [&] { RunFill(j); });
        } else if (j.check) {
            Guard("bones check", [&] {
                Job now = j;
                CaptureImport(now, j.liveBones, j.boneRows);
                g_checked.fetch_add(1, std::memory_order_relaxed);
                if (HashBones(now.current) != j.recordHash) g_changedBySubmit.fetch_add(1, std::memory_order_relaxed);
                if (BadBones(now.current)) g_badAtSubmit.fetch_add(1, std::memory_order_relaxed);
            });
        } else if (j.deferRead) {
            Guard("bones import", [&] {
                Job now = j;
                memcpy(now.config.data(), j.liveConfig, kConfigBytes);
                CaptureImport(now, j.liveBones, j.boneRows);
                if (BadBones(now.current)) {
                    g_badAtSubmit.fetch_add(1, std::memory_order_relaxed);
                    LogBad(now, "at submit");
                }
                RunImport(now);
            });
        } else {
            Guard("bones import", [&] { RunImport(j); });
        }
    }
}

int CycleMode() {
    const int mode = (g_mode.load() + 1) % 8;
    g_mode.store(mode);
    LogInfo("bones mode %d: config %s, bones %s, the game's bones read %s%s", mode, mode & 1 ? "at submit" : "as recorded",
            mode & 2 ? "the newest" : "of the frame recorded in", mode & 4 ? "at submit" : "as recorded", mode == 0 ? " (the default)" : "");
    return mode;
}

void LogStats() {
    if (!g_any.load(std::memory_order_relaxed)) return;
    LogInfo("merged skeleton: %llu bone imports, %llu mod draws filled (%llu with all bones zero); %llu draws with no instance config found; "
            "ring peak %llu KB of %llu a frame",
            (unsigned long long)g_imports.load(), (unsigned long long)g_fills.load(), (unsigned long long)g_zeroFills.load(),
            (unsigned long long)g_noConfig.load(),
            (unsigned long long)(g_ringPeak.load() >> 10), (unsigned long long)(kSegmentBytes >> 10));
    LogInfo("merged skeleton: the game's bones read as recorded: %llu checked at submit, %llu changed by then; impossible values: %llu as "
            "recorded, %llu at submit; mod draws given impossible bones %llu, an impossible transform %llu",
            (unsigned long long)g_checked.load(), (unsigned long long)g_changedBySubmit.load(), (unsigned long long)g_badAtRecord.load(),
            (unsigned long long)g_badAtSubmit.load(), (unsigned long long)g_badFills.load(), (unsigned long long)g_badConfigs.load());
    LogInfo("skin check: %llu mod draws' vertices skinned on the CPU, %llu with a vertex more than 5 m from its object",
            (unsigned long long)g_skinChecks.load(), (unsigned long long)g_skinBad.load());
    LogInfo("merged skeleton: %llu draws whose vertex shader reads its bones elsewhere than the old guess (now followed)",
            (unsigned long long)g_bonesGuessWrong.load());
}

void OnPresent() {
    const uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    // The segment this frame writes was last written kSegments frames ago.
    const VkDeviceSize used = g_cursor[(frame - 1) % kSegments].load(std::memory_order_relaxed);
    if (used > g_ringPeak.load(std::memory_order_relaxed)) g_ringPeak.store(used, std::memory_order_relaxed);
    g_cursor[frame % kSegments].store(0, std::memory_order_relaxed);
    static uint64_t lastFull = 0;
    const uint64_t full = g_ringFull.load(std::memory_order_relaxed);
    if (full != lastFull && frame % 600 == 0) {
        LogError("merged skeleton: the bone ring was full %llu times; those draws were left to the game", (unsigned long long)(full - lastFull));
        lastFull = full;
    }
}

}  // namespace efmivulkan::efmibones
