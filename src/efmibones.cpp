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
constexpr uint32_t kSegments = 8;  // Some command buffers are submitted 3 frames after recording.
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
    const uint8_t* config = nullptr;
    // Import.
    uint32_t component = 0;
    int lod = 0;
    const uint8_t* bones = nullptr;
    uint64_t boneRows = 0;
    uint64_t frame = 0;
    uint32_t slot = 0;  // The spatial-identification slot: the skeleton region.
    // Fill.
    const std::vector<uint16_t>* palette = nullptr;
    VkDeviceSize configAt = 0, currentAt = 0, previousAt = 0;
};

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

// MergedSkeleton_BoneDataImporter_RollingBuffer.hlsl: the component's bones from the game's
// buffer at the instance config's row 5 offsets (each only when past the static rows), LoD
// remapped, scaled by the mod's mesh scale, into the slot's region.
void RunImport(const Job& j) {
    const uint32_t current = Row5(j.config, 0), previous = Row5(j.config, 1);
    g_imports.fetch_add(1, std::memory_order_relaxed);
    Skeleton& s = *j.s;
    if (j.component >= s.components.size() || j.slot >= kMaxInstances) return;
    const Component& c = s.components[j.component];
    std::lock_guard lock(s.mu);
    SkeletonInstance& in = s.instances[j.slot];
    const std::vector<uint16_t>* remap = j.lod > 0 && j.lod < static_cast<int>(kMaxLods) && !c.lodRemap[j.lod].empty() ? &c.lodRemap[j.lod] : nullptr;
    const float scale = s.meshScale;
    const auto copy = [&](float* to, uint64_t row) {
        memcpy(to, j.bones + row * 16, 48);
        if (scale != 1.0f) {
            for (int k = 0; k < 12; ++k) to[k] *= scale;
        }
    };
    for (uint32_t b = 0; b < c.count && c.offset + b < s.bones; ++b) {
        const uint32_t source = remap ? (b < remap->size() ? (*remap)[b] : b) : b;
        if (current >= kStaticRows) {
            const uint64_t row = uint64_t(current) + 3 + 3ull * source;
            if (row + 3 <= j.boneRows) copy(&in.current[size_t(c.offset + b) * 12], row);
        }
        if (previous >= kStaticRows) {
            const uint64_t row = uint64_t(previous) + 3 + 3ull * source;
            if (row + 3 <= j.boneRows) copy(&in.previous[size_t(c.offset + b) * 12], row);
        }
    }
}

// MergedSkeleton_InstanceConfigOverrider_RollingBuffer.hlsl: the instance's config with row 5
// pointing at the merged bones (each offset only when it was past the static rows), and the
// slot's bones this mod draw's palette uses (the 8-bit indices Vulkan's pipelines read).
void RunFill(const Job& j) {
    g_fills.fetch_add(1, std::memory_order_relaxed);
    uint8_t* config = g_ringMapped + j.configAt;
    memcpy(config, j.config, kConfigBytes);
    uint32_t rows[2] = {Row5(j.config, 0), Row5(j.config, 1)};
    if (rows[0] >= kStaticRows) rows[0] = static_cast<uint32_t>(j.currentAt / 16);
    if (rows[1] >= kStaticRows) rows[1] = static_cast<uint32_t>(j.previousAt / 16);
    memcpy(config + kBoneOffsetsRow * 16, rows, sizeof(rows));
    Skeleton& s = *j.s;
    if (j.slot >= kMaxInstances) return;
    std::lock_guard lock(s.mu);
    const SkeletonInstance& in = s.instances[j.slot];
    for (int which = 0; which < 2; ++which) {
        uint8_t* block = g_ringMapped + (which ? j.previousAt : j.currentAt);
        const std::vector<float>& from = which ? in.previous : in.current;
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
        in.current.assign(size_t(bones) * 12, 0.0f);
        in.previous = in.current;
        in.updateFrame.assign(components.size(), UINT64_MAX);
        in.lodLevel.assign(components.size(), 0);
    }
}

void SetDevice(const DeviceFns& fns) { g_fns = fns; }
void SetLookups(const Lookups& l) { g_lookups = l; }

bool FindBoneBinding(const efmitex::BoundSet* sets, BoneBinding* out) {
    if (!g_lookups.Contents || !g_lookups.Mapped) return false;
    efmitex::SetContents c;
    bool bones = false;
    VkDeviceSize bonesSize = 0;
    struct Candidate {
        uint32_t set, binding, element;
        const uint8_t* config;
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
                if (uniform && range == kConfigRange && offset + kConfigBytes <= size) {
                    if (configCount < 8) configs[configCount++] = {i, e.dstBinding, e.dstArrayElement + k, mapped + offset};
                } else if (!uniform && offset == 0 && size > bonesSize) {
                    bones = true;
                    bonesSize = size;
                    out->bonesSet = i;
                    out->bonesBinding = e.dstBinding;
                    out->bonesElement = e.dstArrayElement + k;
                    out->bones = mapped;
                    out->boneRows = size / 16;
                }
            }
        }
    }
    if (!bones) return false;
    for (uint32_t k = 0; k < configCount; ++k) {
        if (!ConfigValid(configs[k].config, out->boneRows)) continue;
        out->configSet = configs[k].set;
        out->configBinding = configs[k].binding;
        out->configElement = configs[k].element;
        out->config = configs[k].config;
        return true;
    }
    g_noConfig.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void Import(VkCommandBuffer /*cb*/, Skeleton* s, uint32_t component, int lod, const BoneBinding& b, uint32_t slot, const uint8_t* config) {
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
    j.config = config;
    j.component = component;
    j.lod = lod;
    j.bones = b.bones;
    j.boneRows = b.boneRows;
    j.frame = frame;
    j.slot = slot;
    // Copied now, as the draw is recorded: the game has written this draw's bones and config
    // by then (EFMI's importer reads them at the draw, on the GPU). At submit the config's
    // memory can already hold another frame's.
    RunImport(j);
    g_any.store(true, std::memory_order_relaxed);
}

bool Redirect(VkCommandBuffer cb, Skeleton* s, uint32_t component, int lod, const std::vector<uint16_t>* palette, const BoneBinding& b,
              uint32_t slot, const uint8_t* config, efmitex::BufferPatch patches[2]) {
    if (!CreateRing()) return false;
    const VkDeviceSize blockBytes = (3 + 3 * VkDeviceSize(palette->size())) * 16;
    const VkDeviceSize configAt = Allocate(kConfigBytes), currentAt = Allocate(blockBytes), previousAt = Allocate(blockBytes);
    if (configAt == UINT64_MAX || currentAt == UINT64_MAX || previousAt == UINT64_MAX) return false;
    Job j;
    j.fill = true;
    j.s = s;
    j.component = component;
    j.lod = lod;
    j.config = config;
    j.palette = palette;
    j.boneRows = b.boneRows;
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
        } else {
            Guard("bones import", [&] { RunImport(j); });
        }
    }
}

void LogStats() {
    if (!g_any.load(std::memory_order_relaxed)) return;
    LogInfo("merged skeleton: %llu bone imports, %llu mod draws filled (%llu with all bones zero); %llu draws with no instance config found; "
            "ring peak %llu KB of %llu a frame",
            (unsigned long long)g_imports.load(), (unsigned long long)g_fills.load(), (unsigned long long)g_zeroFills.load(),
            (unsigned long long)g_noConfig.load(),
            (unsigned long long)(g_ringPeak.load() >> 10), (unsigned long long)(kSegmentBytes >> 10));
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
