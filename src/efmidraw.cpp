#include "efmidraw.h"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "common.h"
#include "efmi.h"
#include "efmimod.h"
#include "efmibones.h"
#include "efmitex.h"
#include "log.h"
#include "modindex.h"
#include "textures.h"

namespace efmivulkan::efmidraw {
namespace {

constexpr uint64_t kVisibleFrames = 3;           // A mod drawn this recently is on screen, for its keys.
constexpr VkDeviceSize kRebarMinHeap = 1ull << 30;  // A CPU-writable VRAM heap this big is Resizable BAR.

DeviceFns g_fns;
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_hideReplaced{false};
std::atomic<int> g_hideDraw{-1};  // Alt+N: the draw (by its place in a list) each mod list leaves out; -1 none.  // Alt+H: replaced draws draw nothing, to see what is left.

struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint32_t stride = 0, indexBytes = 0;
    const uint8_t* mapped = nullptr;  // Its bytes, for trimmed copies.
};

// A mod vertex buffer with each element cut to a shorter stride the game reads: EFMI's LoD 1
// buffers are its LoD 0 ones cut this way (UVs without the 4 bytes after them, 12 -> 8).
// Made on the job thread the first time a draw needs one.
struct Trimmed {
    std::atomic<int> state{0};  // 0 free, 1 being made, 2 ready, 3 failed.
    std::atomic<int> source{-1};  // Set last: readers match on it without the lock.
    uint32_t stride = 0;
    // Repacked: the mod's attributes, packed back to back as DX11 has them, each moved to
    // the offset the Vulkan pipeline reads it at (count 0: a plain trim or pad).
    AttributeLayout repack;
    GpuBuffer gpu;
};
constexpr int kMaxTrimmed = 32;

enum class Load { kNone, kLoading, kReady, kFailed };

struct Runtime {
    std::string name;
    std::wstring ini, userIni;
    std::vector<EfmiBuffer> files;  // As first parsed; buffer indices never change.
    std::vector<GpuBuffer> gpu;
    std::vector<std::vector<uint8_t>> indexData;  // CPU copies of the index buffers, for the checks.
    std::vector<std::wstring> textureFiles;
    std::vector<uint32_t> textureHashes;  // The game texture each replaces, 0: none named.
    std::map<uint32_t, int> thisByHash;   // Game texture hash to texture index.
    std::vector<int> textureIds;          // efmitex ids, once loaded.
    // Merged skeleton (gpu_posed mods), made when the buffers load.
    uint32_t bonesCount = 0;
    std::vector<EfmiSkeletonComponent> skeletonComponents;
    efmibones::Skeleton skeleton;
    bool hasSkeleton = false;
    // Bone records (Mifu's cross-IB): 256 bones per ID, from the draw recording them.
    efmibones::Skeleton records;
    size_t recordCount = 0;
    std::vector<std::vector<uint16_t>> recordPalettes;
    Trimmed trimmed[kMaxTrimmed];
    std::mutex trimMu;
    // EFMI's spatial identification (SpatialIdentification.ini), when the mod runs it: the
    // mod's Pool_ObjectSpatialIdentity, a slot per object found by position, with its
    // identification state. The slot index is the object's "Spatial ID".
    bool spatialIdentification = false;
    int minComponents = 4;  // $\EFMIv1\identification_min_components, EFMI's default 4.
    std::mutex spatialMu;
    struct SpatialSlot {
        bool used = false;
        float position[3]{};
        uint64_t lastRead = 0;  // Expires 1 frame after (pool_expiration_timeout_frames = 1).
        enum State { kEmpty, kTracking, kCustom, kUnknown } state = kEmpty;
        uint64_t snapshotFrame = 0;  // First encounter (the bitmask's FrameSnapshot).
        uint16_t componentBits = 0;  // Components seen, ids wrapped around 16.
    } spatial[efmibones::kMaxInstances];
    std::vector<std::vector<uint16_t>> palettes;  // Per buffer with bone indices: its bone -> merged bone.
    std::atomic<Load> load{Load::kNone};
    // Replaced as a whole when a toggle changes them; a draw keeps the list it started with.
    std::atomic<std::shared_ptr<const std::vector<EfmiComponent>>> components;
    std::vector<ini::ModKey> keys;
    std::mutex toggleMu;
    std::map<std::string, int> toggles;
    std::vector<char> keyDown;  // Input thread only.
    std::atomic<uint64_t> lastDrawFrame{0};
};

struct Target {
    Runtime* rt;
    size_t component;
};

std::atomic<bool> g_loaded{false};
std::vector<Runtime*>* g_runtimes = new std::vector<Runtime*>();
std::unordered_map<uint32_t, std::vector<Target>>* g_byHash = new std::unordered_map<uint32_t, std::vector<Target>>();

std::mutex g_logMu;
std::set<std::tuple<const Runtime*, size_t, int>>* g_logged = new std::set<std::tuple<const Runtime*, size_t, int>>();

enum Reason { kGpuPosed, kInstanced, kStride, kNoBuffer, kNoPipeline, kReplaced, kTextures, kOtherBones, kNoBones, kRingFull, kSkeleton, kRecords, kUnidentified, kTrimming };

void LogOnce(const Runtime* rt, size_t component, Reason reason, const char* fmt, ...) {
    // Called on every matched draw: each thread skips what it has seen without a lock.
    thread_local std::set<std::tuple<const Runtime*, size_t, int>> seen;
    if (!seen.insert({rt, component, reason}).second) return;
    {
        std::lock_guard lock(g_logMu);
        if (!g_logged->insert({rt, component, reason}).second) return;
    }
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    LogInfo("%s [%s]: %s", rt->name.c_str(), (*rt->components.load())[component].section.c_str(), text);
}

// ---- Toggles saved across runs: one line per variable, <mod>|<$variable>=<value>.

std::mutex g_savedMu;
std::map<std::string, std::map<std::string, int>>* g_saved = nullptr;

std::wstring TogglesFile() { return LogDir() + L"\\toggles.ini"; }

std::map<std::string, int> SavedToggles(const std::string& mod) {
    std::lock_guard lock(g_savedMu);
    if (!g_saved) {
        g_saved = new std::map<std::string, std::map<std::string, int>>();
        std::ifstream f(TogglesFile());
        std::string line;
        while (std::getline(f, line)) {
            const size_t bar = line.find('|'), eq = line.rfind('=');
            if (bar == std::string::npos || eq == std::string::npos || eq < bar) continue;
            (*g_saved)[line.substr(0, bar)][line.substr(bar + 1, eq - bar - 1)] = std::atoi(line.c_str() + eq + 1);
        }
    }
    const auto it = g_saved->find(mod);
    return it == g_saved->end() ? std::map<std::string, int>() : it->second;
}

void SaveToggles(const std::string& mod, const std::map<std::string, int>& toggles) {
    std::lock_guard lock(g_savedMu);
    if (!g_saved) return;
    (*g_saved)[mod] = toggles;
    std::ofstream f(TogglesFile(), std::ios::trunc);
    for (const auto& [name, vars] : *g_saved) {
        for (const auto& [var, value] : vars) f << name << '|' << var << '=' << value << '\n';
    }
}

// ---- Checks done when a draw list is made, so the draw path has none of them.

// Drops draws that would read past the end of the index buffer or of a vertex buffer.
std::vector<EfmiComponent> Checked(const Runtime& rt, std::vector<EfmiComponent> components) {
    for (EfmiComponent& c : components) {
        EfmiDrawList& list = c.list;
        if (list.draws.empty()) continue;
        std::vector<EfmiDrawCall> kept;
        for (const EfmiDrawCall& d : list.draws) {
            if (!d.indexCount) continue;  // drawindexed = 0, 0, 0: a placeholder some mods keep.
            if (d.ib < 0 || !rt.files[d.ib].indexBytes) {
                LogError("%s [%s]: a draw with no usable index buffer; skipped", rt.name.c_str(), c.section.c_str());
                continue;
            }
            uint64_t vertices = UINT64_MAX;
            for (int vb : d.vb) {
                if (vb >= 0 && rt.files[vb].stride) vertices = std::min<uint64_t>(vertices, rt.gpu[vb].size / rt.files[vb].stride);
            }
            const std::vector<uint8_t>& indices = rt.indexData[d.ib];
            const uint32_t bytes = rt.files[d.ib].indexBytes;
            const uint64_t count = indices.size() / bytes;
            bool ok = uint64_t(d.firstIndex) + d.indexCount <= count;
            int64_t highest = -1;
            for (uint64_t i = d.firstIndex; ok && i < uint64_t(d.firstIndex) + d.indexCount; ++i) {
                const uint32_t v = bytes == 4 ? reinterpret_cast<const uint32_t*>(indices.data())[i] : reinterpret_cast<const uint16_t*>(indices.data())[i];
                highest = std::max<int64_t>(highest, v);
            }
            ok = ok && highest + d.baseVertex < int64_t(vertices) && highest + d.baseVertex >= 0;
            if (ok) {
                kept.push_back(d);
            } else {
                LogError("%s [%s]: drawindexed = %u, %u, %d reads past its buffers; skipped", rt.name.c_str(), c.section.c_str(), d.indexCount,
                         d.firstIndex, d.baseVertex);
            }
        }
        list.draws = std::move(kept);
    }

    // Duplicates. A DX11 mod draws some pieces from two components under different shader
    // filters ("if vs == 200" in one, "if vs == 202" in the other: Mifu's and Chen's bodies),
    // so each pass has one. The layer cannot tell those shaders apart and would draw both in
    // every pass: one on the draw's own bones, one on bones another component recorded,
    // which can be a frame old, so the two shake against each other. The copy on recorded
    // bones goes where some component draws the same piece on its own bones; and a list
    // that draws the same piece twice (inside and after a filtered block) draws it once.
    const auto key = [](const EfmiDrawCall& d) {
        return std::make_tuple(d.ib, d.firstIndex, d.indexCount, d.baseVertex, d.vb);
    };
    std::set<decltype(key(EfmiDrawCall{}))> own;
    for (const EfmiComponent& c : components) {
        for (const EfmiDrawCall& d : c.list.draws) {
            if (d.boneSource < 0) own.insert(key(d));
        }
    }
    for (EfmiComponent& c : components) {
        std::set<std::tuple<decltype(key(EfmiDrawCall{})), int>> seen;
        std::vector<EfmiDrawCall> kept;
        uint32_t dropped = 0;
        for (const EfmiDrawCall& d : c.list.draws) {
            if ((d.boneSource >= 0 && own.count(key(d))) || !seen.insert({key(d), d.boneSource}).second) {
                ++dropped;
                continue;
            }
            kept.push_back(d);
        }
        if (dropped) {
            LogInfo("%s [%s]: %u draws left out, drawn already (by this list, or by a component on its own bones)", rt.name.c_str(), c.section.c_str(),
                    dropped);
        }
        c.list.draws = std::move(kept);
    }
    return components;
}

// ---- Spatial identification, as EFMI's SpatialIdentification.ini

// $cfg_spatial_cell_size 0.125 and pool_spatial_radius $cfg_spatial_base_radius 3: positions
// within 3 cells are the same object.
constexpr float kSpatialRadius = 3 * 0.125f;
constexpr uint64_t kIdentificationTimeout = 255;  // $identification_timeout_frames.
constexpr uint64_t kSpatialExpireFrames = 4;  // pool_expiration_timeout_frames 1, in presents (see Identify).

enum class Override { kSkip, kOriginal, kCustom };  // $enum_override_type_*.
std::atomic<uint64_t> g_identified[3]{};  // Instances skipped, left to the game, drawn by the mod.

// CommandListSpatialIdentity_IdentifyObject for one instance: what to draw, and its slot.
Override Identify(Runtime* rt, int componentId, const uint8_t* config, uint64_t frame, uint32_t* slot) {
    *slot = 0;
    if (!rt->spatialIdentification || !config) return Override::kCustom;
    float p[3];
    memcpy(p, config + 3 * 16, sizeof(p));  // Instance config values 12, 13, 14.
    std::lock_guard lock(rt->spatialMu);
    Runtime::SpatialSlot* found = nullptr;
    float best = kSpatialRadius * kSpatialRadius;
    for (Runtime::SpatialSlot& s : rt->spatial) {
        if (!s.used) continue;
        const float dx = s.position[0] - p[0], dy = s.position[1] - p[1], dz = s.position[2] - p[2];
        const float d = dx * dx + dy * dy + dz * dz;
        if (d <= best) {
            best = d;
            found = &s;
        }
    }
    // EFMI expires a slot after 1 frame unseen. Frames here are counted by presents, and the
    // game can present more than once a frame: with 1, every object expired between its
    // frames and was skipped as new every time (invisible). A few presents of leeway.
    const auto expired = [&](const Runtime::SpatialSlot& s) { return s.used && frame > s.lastRead + kSpatialExpireFrames; };
    if (!found) {
        // pool_allocate_slot_on_missing: a free slot, or an expired one.
        for (Runtime::SpatialSlot& s : rt->spatial) {
            if (!s.used || expired(s)) {
                found = &s;
                break;
            }
        }
        if (!found) return Override::kOriginal;  // The pool is full: not tracked.
        *found = Runtime::SpatialSlot{};
        found->used = true;
    } else if (expired(*found)) {
        // pool_expiration_reset_elements: its value back to the default (0).
        found->state = Runtime::SpatialSlot::kEmpty;
        found->componentBits = 0;
    }
    // The slot follows its object: the position it was last seen at. Kept where first seen, a
    // moving character left its slot every 0.375 m and came back as a new object, skipped
    // for a frame and with no bones yet: invisible for a frame, again and again.
    memcpy(found->position, p, sizeof(p));
    found->lastRead = std::max(found->lastRead, frame);  // pool_expiration_refresh_on_read.
    *slot = static_cast<uint32_t>(found - rt->spatial);
    switch (found->state) {
        case Runtime::SpatialSlot::kCustom:
            return Override::kCustom;
        case Runtime::SpatialSlot::kUnknown:
            return Override::kOriginal;
        default:
            break;
    }
    // CommandList_SpatialIdentity_AdvanceOngoingIdentification.
    uint64_t distance = 0;
    if (found->state == Runtime::SpatialSlot::kEmpty) {
        found->state = Runtime::SpatialSlot::kTracking;
        found->snapshotFrame = frame;
    } else {
        distance = frame > found->snapshotFrame ? frame - found->snapshotFrame : 0;
    }
    found->componentBits |= uint16_t(1u << ((componentId < 0 ? 0 : componentId) & 0xF));
    if (distance == 0) return Override::kSkip;  // Its first frame: drawn by no one.
    if (__popcnt16(found->componentBits) >= rt->minComponents) {
        found->state = Runtime::SpatialSlot::kCustom;
        return Override::kCustom;
    }
    if (distance >= kIdentificationTimeout) found->state = Runtime::SpatialSlot::kUnknown;
    return Override::kOriginal;
}

// ---- GPU buffers

// Memory for vertex and index data: VRAM the CPU can write (Resizable BAR) if the GPU
// offers a big enough heap of it, else system memory the GPU reads over PCIe.
int MemoryType(uint32_t bits, bool* vram) {
    const VkPhysicalDeviceMemoryProperties& m = g_fns.memory;
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < m.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags f = m.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (f & host) == host &&
            m.memoryHeaps[m.memoryTypes[i].heapIndex].size >= kRebarMinHeap) {
            *vram = true;
            return static_cast<int>(i);
        }
    }
    for (uint32_t i = 0; i < m.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & host) == host) {
            *vram = false;
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool ReadFile(const std::wstring& path, std::vector<uint8_t>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out->empty();
}

// ---- Merged skeleton

// The gpu_posed components' bone index buffers (vb2) have 16-bit indices into the merged
// skeleton; the game's pipelines read 8-bit ones. Each such buffer gets a palette of the
// merged bones it uses (at most 256) and 8-bit indices into it, in the game's layout:
//   weights R16G16B16A16_UNORM + indices R16G16B16A16_UINT, 16 bytes -> + R8G8B8A8_UINT, 12
//   weights R32G32B32A32_FLOAT + indices R16G16B16A16_UINT, 24 bytes -> + R8G8B8A8_UINT, 20
//   indices R16G16B16A16_UINT, 8 bytes                              -> R8G8B8A8_UINT, 4
// (The weights as the game has them; EFMI widens only the indices.)
void ConvertBoneIndices(Runtime* rt, std::vector<std::vector<uint8_t>>& data) {
    std::set<int> buffers;
    for (const EfmiComponent& c : *rt->components.load()) {
        if (!c.gpuPosed) continue;
        for (const EfmiDrawCall& d : c.list.draws) {
            if (d.vb[2] >= 0) buffers.insert(d.vb[2]);
        }
    }
    rt->palettes.assign(rt->files.size(), {});
    for (const int i : buffers) {
        EfmiBuffer& f = rt->files[i];
        const bool weights = f.stride == 16 || f.stride == 24;
        const uint32_t weightBytes = weights ? f.stride - 8 : 0, weightSize = weightBytes / 4;
        if (!(weights || (f.stride == 8 && f.format.find("uint") != std::string::npos))) {
            LogError("%s: %s has stride %u, not bone weights and indices EFMI writes; its components stay the game's", rt->name.c_str(),
                     f.name.c_str(), f.stride);
            continue;
        }
        const std::vector<uint8_t>& in = data[i];
        const size_t vertices = in.size() / f.stride;
        const uint32_t indexAt = weightBytes;
        std::map<uint16_t, uint8_t> local;
        std::vector<uint16_t> palette;
        bool tooMany = false;
        for (size_t v = 0; v < vertices && !tooMany; ++v) {
            const uint8_t* p = in.data() + v * f.stride;
            for (int k = 0; k < 4; ++k) {
                uint32_t w = 1;
                uint16_t m;
                if (weights) {
                    w = 0;
                    memcpy(&w, p + weightSize * k, weightSize);
                }
                memcpy(&m, p + indexAt + 2 * k, 2);
                if (!w || local.count(m)) continue;
                if (palette.size() == 256) {
                    tooMany = true;
                    break;
                }
                local[m] = static_cast<uint8_t>(palette.size());
                palette.push_back(m);
            }
        }
        if (tooMany) {
            LogError("%s: %s uses more than 256 bones; its components stay the game's", rt->name.c_str(), f.name.c_str());
            continue;
        }
        // Float weights: the game reads R32G32B32A32_UINT indices after them (seen in game,
        // a 32-byte binding); 16-bit weights: R8G8B8A8_UINT.
        const uint32_t indexBytes = weightBytes == 16 ? 4 : 1;
        const uint32_t stride = weightBytes + 4 * indexBytes;
        std::vector<uint8_t> out(vertices * stride);
        for (size_t v = 0; v < vertices; ++v) {
            const uint8_t* p = in.data() + v * f.stride;
            uint8_t* q = out.data() + v * stride;
            if (weights) memcpy(q, p, weightBytes);
            for (int k = 0; k < 4; ++k) {
                uint16_t m;
                memcpy(&m, p + indexAt + 2 * k, 2);
                const auto it = local.find(m);
                const uint32_t value = it == local.end() ? 0 : it->second;
                memcpy(q + weightBytes + k * indexBytes, &value, indexBytes);  // Unweighted: any bone.
            }
        }
        data[i] = std::move(out);
        f.stride = stride;
        rt->palettes[i] = std::move(palette);
        LogInfo("%s: %s: %zu vertices, %zu bones of the merged skeleton", rt->name.c_str(), f.name.c_str(), vertices, rt->palettes[i].size());
    }
}

void MakeSkeleton(Runtime* rt, const std::vector<std::vector<uint8_t>>& data) {
    if (!rt->bonesCount || rt->skeletonComponents.empty()) return;
    std::vector<efmibones::Component> components(rt->skeletonComponents.size());
    for (size_t c = 0; c < components.size(); ++c) {
        const EfmiSkeletonComponent& sc = rt->skeletonComponents[c];
        if (sc.offset < 0) continue;
        components[c].offset = static_cast<uint32_t>(sc.offset);
        components[c].count = static_cast<uint32_t>(sc.count);
        for (uint32_t lod = 0; lod < efmibones::kMaxLods; ++lod) {
            const int b = sc.lodRemap[lod];
            if (b < 0) continue;
            std::vector<uint16_t>& remap = components[c].lodRemap[lod];
            remap.resize(data[b].size() / 2);
            memcpy(remap.data(), data[b].data(), remap.size() * 2);
        }
    }
    rt->skeleton.Init(rt->bonesCount, std::move(components));
    rt->hasSkeleton = true;
    LogInfo("%s: merged skeleton of %u bones, %zu components", rt->name.c_str(), rt->bonesCount, rt->skeletonComponents.size());
}

// A record copies a draw's first 256 bones, as Mifu's record shader copies 768 rows.
constexpr uint32_t kRecordBones = 256;

void MakeRecords(Runtime* rt) {
    if (!rt->recordCount) return;
    std::vector<efmibones::Component> components(rt->recordCount);
    rt->recordPalettes.assign(rt->recordCount, {});
    for (size_t i = 0; i < rt->recordCount; ++i) {
        components[i].offset = static_cast<uint32_t>(i) * kRecordBones;
        components[i].count = kRecordBones;
        for (uint32_t b = 0; b < kRecordBones; ++b) rt->recordPalettes[i].push_back(static_cast<uint16_t>(i * kRecordBones + b));
    }
    rt->records.Init(static_cast<uint32_t>(rt->recordCount * kRecordBones), std::move(components));
    LogInfo("%s: %zu bone records (cross-IB bones)", rt->name.c_str(), rt->recordCount);
}

// One allocation for all of a mod's buffers, written once by the CPU.
void LoadGpu(Runtime* rt) {
    rt->textureIds = efmitex::Load(rt->name, rt->textureFiles);
    if (rt->files.empty()) {  // Only hides components: nothing to load.
        rt->load.store(Load::kReady, std::memory_order_release);
        return;
    }
    std::vector<std::vector<uint8_t>> data(rt->files.size());
    for (size_t i = 0; i < rt->files.size(); ++i) {
        if (!ReadFile(rt->files[i].path, &data[i])) {
            LogError("%s: cannot read %s", rt->name.c_str(), ToUtf8(rt->files[i].path).c_str());
            rt->load.store(Load::kFailed);
            return;
        }
    }
    MakeSkeleton(rt, data);
    MakeRecords(rt);
    ConvertBoneIndices(rt, data);
    std::vector<GpuBuffer> gpu(rt->files.size());
    std::vector<VkDeviceSize> offsets(rt->files.size());
    VkDeviceSize total = 0;
    uint32_t bits = ~0u;
    for (size_t i = 0; i < rt->files.size(); ++i) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = data[i].size();
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (g_fns.CreateBuffer(g_fns.device, &info, nullptr, &gpu[i].buffer) != VK_SUCCESS) {
            rt->load.store(Load::kFailed);
            LogError("%s: vkCreateBuffer failed", rt->name.c_str());
            return;
        }
        VkMemoryRequirements req;
        g_fns.GetBufferMemoryRequirements(g_fns.device, gpu[i].buffer, &req);
        total = (total + req.alignment - 1) / req.alignment * req.alignment;
        offsets[i] = total;
        total += req.size;
        bits &= req.memoryTypeBits;
        gpu[i].size = data[i].size();
        gpu[i].stride = rt->files[i].stride;
        gpu[i].indexBytes = rt->files[i].indexBytes;
    }
    bool vram = false;
    const int type = MemoryType(bits, &vram);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = total;
    alloc.memoryTypeIndex = static_cast<uint32_t>(type);
    VkDeviceMemory memory;
    void* mapped = nullptr;
    if (type < 0 || g_fns.AllocateMemory(g_fns.device, &alloc, nullptr, &memory) != VK_SUCCESS ||
        g_fns.MapMemory(g_fns.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        LogError("%s: no memory for %llu MB of buffers", rt->name.c_str(), static_cast<unsigned long long>(total >> 20));
        rt->load.store(Load::kFailed);
        return;
    }
    for (size_t i = 0; i < gpu.size(); ++i) {
        g_fns.BindBufferMemory(g_fns.device, gpu[i].buffer, memory, offsets[i]);
        memcpy(static_cast<uint8_t*>(mapped) + offsets[i], data[i].data(), data[i].size());
        gpu[i].mapped = static_cast<uint8_t*>(mapped) + offsets[i];
    }
    rt->gpu = std::move(gpu);
    rt->indexData.assign(rt->files.size(), {});
    for (size_t i = 0; i < rt->files.size(); ++i) {
        if (rt->files[i].indexBytes) rt->indexData[i] = std::move(data[i]);
    }
    rt->components.store(std::make_shared<const std::vector<EfmiComponent>>(Checked(*rt, *rt->components.load())));
    rt->load.store(Load::kReady, std::memory_order_release);
    LogInfo("%s: %zu buffers loaded, %llu MB in %s", rt->name.c_str(), rt->gpu.size(), static_cast<unsigned long long>(total >> 20),
            vram ? "VRAM (Resizable BAR)" : "system memory: no CPU-writable VRAM heap, so the GPU reads them over PCIe");
}

// Position + normal (24 bytes, the DX11 layout EFMI exports) to the 40 bytes the Vulkan
// build reads: + a float4 tangent (xyz, w the bitangent's sign), computed from the mod's
// triangles and UVs (its vb1, R32G32 first) as Blender would, since the mod has none.
bool AddTangents(Runtime* rt, int source, uint8_t* out, VkDeviceSize vertices) {
    struct V3 {
        float x = 0, y = 0, z = 0;
    };
    std::vector<V3> tan(vertices), bit(vertices);
    const GpuBuffer& pos = rt->gpu[source];
    const auto P = [&](uint32_t v, int k) {
        float f;
        memcpy(&f, pos.mapped + size_t(v) * pos.stride + 4 * k, 4);
        return f;
    };
    bool any = false;
    for (const EfmiComponent& c : *rt->components.load()) {
        for (const EfmiDrawCall& d : c.list.draws) {
            if (d.vb[0] != source || d.ib < 0 || d.vb[1] < 0 || !rt->gpu[d.vb[1]].mapped || rt->gpu[d.vb[1]].stride < 8) continue;
            const std::vector<uint8_t>& ib = rt->indexData[d.ib];
            const uint32_t bytes = rt->files[d.ib].indexBytes;
            if (!bytes) continue;
            const GpuBuffer& uvs = rt->gpu[d.vb[1]];
            const VkDeviceSize uvCount = uvs.size / uvs.stride;
            const auto U = [&](uint32_t v, int k) {
                float f;
                memcpy(&f, uvs.mapped + size_t(v) * uvs.stride + 4 * k, 4);
                return f;
            };
            for (uint32_t i = 0; i + 2 < d.indexCount; i += 3) {
                uint32_t v[3];
                bool ok = true;
                for (int k = 0; k < 3; ++k) {
                    const size_t at = (size_t(d.firstIndex) + i + k) * bytes;
                    if (at + bytes > ib.size()) {
                        ok = false;
                        break;
                    }
                    uint32_t x = 0;
                    memcpy(&x, ib.data() + at, bytes);
                    const int64_t vv = int64_t(x) + d.baseVertex;
                    if (vv < 0 || uint64_t(vv) >= vertices || uint64_t(vv) >= uvCount) ok = false;
                    v[k] = static_cast<uint32_t>(vv);
                }
                if (!ok) continue;
                const float e1[3] = {P(v[1], 0) - P(v[0], 0), P(v[1], 1) - P(v[0], 1), P(v[1], 2) - P(v[0], 2)};
                const float e2[3] = {P(v[2], 0) - P(v[0], 0), P(v[2], 1) - P(v[0], 1), P(v[2], 2) - P(v[0], 2)};
                const float du1 = U(v[1], 0) - U(v[0], 0), dv1 = U(v[1], 1) - U(v[0], 1);
                const float du2 = U(v[2], 0) - U(v[0], 0), dv2 = U(v[2], 1) - U(v[0], 1);
                const float det = du1 * dv2 - du2 * dv1;
                if (std::fabs(det) < 1e-12f) continue;
                const float r = 1.0f / det;
                const V3 tt{(e1[0] * dv2 - e2[0] * dv1) * r, (e1[1] * dv2 - e2[1] * dv1) * r, (e1[2] * dv2 - e2[2] * dv1) * r};
                const V3 bb{(e2[0] * du1 - e1[0] * du2) * r, (e2[1] * du1 - e1[1] * du2) * r, (e2[2] * du1 - e1[2] * du2) * r};
                for (const uint32_t x : v) {
                    tan[x].x += tt.x, tan[x].y += tt.y, tan[x].z += tt.z;
                    bit[x].x += bb.x, bit[x].y += bb.y, bit[x].z += bb.z;
                }
                any = true;
            }
        }
    }
    for (VkDeviceSize v = 0; v < vertices; ++v) {
        const float n[3] = {P(uint32_t(v), 3), P(uint32_t(v), 4), P(uint32_t(v), 5)};
        float t3[3] = {tan[v].x, tan[v].y, tan[v].z};
        // Gram-Schmidt against the normal; any perpendicular where the UVs give none.
        const float nd = n[0] * t3[0] + n[1] * t3[1] + n[2] * t3[2];
        for (int k = 0; k < 3; ++k) t3[k] -= n[k] * nd;
        float len = std::sqrt(t3[0] * t3[0] + t3[1] * t3[1] + t3[2] * t3[2]);
        if (len < 1e-8f) {
            const float a[3] = {std::fabs(n[0]) < 0.9f ? 1.0f : 0.0f, std::fabs(n[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
            t3[0] = a[1] * n[2] - a[2] * n[1];
            t3[1] = a[2] * n[0] - a[0] * n[2];
            t3[2] = a[0] * n[1] - a[1] * n[0];
            len = std::sqrt(t3[0] * t3[0] + t3[1] * t3[1] + t3[2] * t3[2]);
            if (len < 1e-8f) {
                t3[0] = 1;
                len = 1;
            }
        }
        const float c[3] = {n[1] * t3[2] - n[2] * t3[1], n[2] * t3[0] - n[0] * t3[2], n[0] * t3[1] - n[1] * t3[0]};
        const float w = c[0] * bit[v].x + c[1] * bit[v].y + c[2] * bit[v].z < 0 ? -1.0f : 1.0f;
        const float tangent[4] = {t3[0] / len, t3[1] / len, t3[2] / len, w};
        memcpy(out + v * 40 + 24, tangent, 16);
    }
    return any;
}

void MakeTrimmed(Runtime* rt, Trimmed* t) {
    const GpuBuffer& from = rt->gpu[t->source];
    const VkDeviceSize vertices = from.size / from.stride;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = std::max<VkDeviceSize>(vertices * t->stride, 4);
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VkMemoryRequirements req;
    VkDeviceMemory memory;
    void* mapped = nullptr;
    bool vram = false;
    if (g_fns.CreateBuffer(g_fns.device, &info, nullptr, &t->gpu.buffer) != VK_SUCCESS) {
        LogError("%s: cannot make a trimmed copy of %s", rt->name.c_str(), rt->files[t->source].name.c_str());
        t->state.store(3);
        return;
    }
    g_fns.GetBufferMemoryRequirements(g_fns.device, t->gpu.buffer, &req);
    const int type = MemoryType(req.memoryTypeBits, &vram);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = static_cast<uint32_t>(type);
    if (type < 0 || g_fns.AllocateMemory(g_fns.device, &alloc, nullptr, &memory) != VK_SUCCESS ||
        g_fns.BindBufferMemory(g_fns.device, t->gpu.buffer, memory, 0) != VK_SUCCESS ||
        g_fns.MapMemory(g_fns.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        LogError("%s: no memory for a trimmed copy of %s", rt->name.c_str(), rt->files[t->source].name.c_str());
        t->state.store(3);
        return;
    }
    // From the mod's bytes kept in memory the CPU wrote: read once, so its speed does not matter.
    for (VkDeviceSize v = 0; v < vertices; ++v) {
        uint8_t* to = static_cast<uint8_t*>(mapped) + v * t->stride;
        const uint8_t* in = from.mapped + v * from.stride;
        if (t->repack.count) {
            memset(to, 0, t->stride);
            uint32_t packed = 0;
            for (uint8_t a = 0; a < t->repack.count; ++a) {
                memcpy(to + t->repack.spans[a].offset, in + packed, t->repack.spans[a].size);
                packed += t->repack.spans[a].size;
            }
            continue;
        }
        const uint32_t keep = std::min<uint32_t>(t->stride, from.stride);
        memcpy(to, in, keep);
        if (keep < t->stride) memset(to + keep, 0, t->stride - keep);  // Padding the game does not read.
    }
    if (from.stride == 24 && t->stride == 40) {
        const bool fromUvs = AddTangents(rt, t->source, static_cast<uint8_t*>(mapped), vertices);
        LogInfo("%s: %s: tangents %s", rt->name.c_str(), rt->files[t->source].name.c_str(),
                fromUvs ? "computed from its triangles and UVs" : "made up (no triangles with UVs found)");
    }
    t->gpu.size = vertices * t->stride;
    t->gpu.stride = t->stride;
    t->state.store(2, std::memory_order_release);
    LogInfo("%s: %s copied from stride %u to %u, as the game reads it", rt->name.c_str(), rt->files[t->source].name.c_str(), from.stride, t->stride);
}

// The trimmed copy of a buffer, or null while it is being made (or cannot be).
const GpuBuffer* TrimmedOf(Runtime* rt, int source, uint32_t stride, const AttributeLayout* repack = nullptr) {
    const bool repacked = repack && repack->count;
    for (Trimmed& t : rt->trimmed) {
        if (t.source == source && t.stride == stride && (t.repack.count != 0) == repacked) {
            return t.state.load(std::memory_order_acquire) == 2 ? &t.gpu : nullptr;
        }
    }
    std::lock_guard lock(rt->trimMu);
    for (Trimmed& t : rt->trimmed) {
        if (t.source == source && t.stride == stride && (t.repack.count != 0) == repacked) return nullptr;
        if (t.state.load() != 0) continue;
        t.state.store(1);
        t.stride = stride;
        t.repack = repacked ? *repack : AttributeLayout{};
        t.source.store(source, std::memory_order_release);
        Trimmed* slot = &t;
        PostJob([rt, slot] { Guard("MakeTrimmed", [&] { MakeTrimmed(rt, slot); }); });
        return nullptr;
    }
    return nullptr;
}

// ---- Toggles

void ApplyToggles(Runtime* rt, std::map<std::string, int> toggles) {
    const std::optional<EfmiMod> mod = ParseEfmiMod(rt->ini, rt->name, rt->userIni, &toggles);
    const auto current = rt->components.load();
    if (!mod || mod->components.size() != current->size() || mod->buffers.size() != rt->files.size()) {
        LogError("%s: the toggle changed its components; ignored", rt->name.c_str());
        return;
    }
    rt->components.store(std::make_shared<const std::vector<EfmiComponent>>(
        rt->load.load() == Load::kReady ? Checked(*rt, mod->components) : mod->components));
    SaveToggles(rt->name, toggles);
}

void Press(Runtime* rt, size_t k) {
    const ini::ModKey& key = rt->keys[k];
    if (key.whenVisible && g_stats.presents.load() - rt->lastDrawFrame.load() > kVisibleFrames) return;
    std::map<std::string, int> toggles;
    {
        std::lock_guard lock(rt->toggleMu);
        int& value = rt->toggles[key.var];
        const auto it = std::find(key.values.begin(), key.values.end(), value);
        value = it == key.values.end() || it + 1 == key.values.end() ? key.values.front() : *(it + 1);
        toggles = rt->toggles;
        LogInfo("%s: %s = %d", rt->name.c_str(), key.var.c_str(), value);
    }
    PostJob([rt, toggles] { Guard("ApplyToggles", [&] { ApplyToggles(rt, toggles); }); });
}

}  // namespace

// Every mod's buffers and textures, as soon as the mods are read and the device is ready, as
// 3DMigoto loads its resources at startup: a character's first draw then has them, instead of
// showing the game's model while they load.
std::atomic<bool> g_deviceReady{false}, g_preloaded{false};
void Preload() {
    if (!g_deviceReady.load() || !g_loaded.load(std::memory_order_acquire) || g_preloaded.exchange(true)) return;
    for (Runtime* rt : *g_runtimes) {
        Load expected = Load::kNone;
        if (rt->load.compare_exchange_strong(expected, Load::kLoading)) {
            PostJob([rt] { Guard("LoadGpu", [&] { LoadGpu(rt); }); });
        }
    }
}

void SetDevice(const DeviceFns& fns) { g_fns = fns; }
void OnDeviceReady() {
    g_deviceReady.store(true);
    Preload();
}

void LoadMods() {
    const ModIndex* index = efmi::GetModIndex();
    if (!index) return;
    const std::wstring dir = index->Dir();
    const std::wstring userIni = dir.substr(0, dir.find_last_of(L'\\')) + L"\\d3dx_user.ini";
    std::set<std::string> files;
    std::set<uint32_t> textureHashes;  // The game textures mods replace.
    for (const auto& [hash, entries] : index->All()) {
        for (const ModEntry& e : entries) {
            if (e.kind == HashKind::IbComponent) files.insert(e.file);
            if (e.kind == HashKind::Texture) textureHashes.insert(hash);
        }
    }
    size_t components = 0;
    for (const std::string& file : files) {
        std::wstring relative = ini::Widen(file);
        std::replace(relative.begin(), relative.end(), L'/', L'\\');
        const std::wstring path = dir + L"\\" + relative;
        const std::map<std::string, int> saved = SavedToggles(file);
        std::optional<EfmiMod> mod = ParseEfmiMod(path, file, userIni, &saved);
        if (!mod) continue;
        auto* rt = new Runtime();
        rt->name = file;
        rt->ini = path;
        rt->userIni = userIni;
        rt->files = mod->buffers;
        rt->keys = mod->keys;
        rt->textureFiles = mod->textures;
        rt->textureHashes = mod->textureHashes;
        rt->thisByHash = mod->thisByHash;
        rt->bonesCount = mod->bonesCount;
        rt->recordCount = mod->boneRecordIds.size();
        rt->spatialIdentification = mod->spatialIdentification;
        rt->minComponents = mod->minComponents > 0 ? mod->minComponents : 4;
        rt->skeleton.meshScale = mod->meshScale;
        rt->skeletonComponents = mod->skeletonComponents;
        for (const auto& [hash, texture] : mod->thisByHash) textureHashes.insert(hash);
        rt->toggles = mod->toggles;
        rt->keyDown.assign(rt->keys.size(), 0);
        // EFMI names each texture after the game texture it replaces: "... t=3156b9c5 ...".
        for (const std::wstring& texture : mod->textures) {
            const size_t at = texture.find(L"t=", texture.find_last_of(L'\\'));
            if (at != std::wstring::npos) textureHashes.insert(static_cast<uint32_t>(wcstoul(texture.c_str() + at + 2, nullptr, 16)));
        }
        for (size_t c = 0; c < mod->components.size(); ++c) (*g_byHash)[mod->components[c].hash].push_back({rt, c});
        components += mod->components.size();
        rt->components.store(std::make_shared<const std::vector<EfmiComponent>>(std::move(mod->components)));
        g_runtimes->push_back(rt);
    }
    textureHashes.erase(0);
    textures::SetTargets(std::move(textureHashes));
    g_loaded.store(true, std::memory_order_release);
    LogInfo("efmi mods: %zu with %zu components, ready to draw", g_runtimes->size(), components);
    Preload();
}

bool TryReplace(VkCommandBuffer cb, const Draw& draw, const BindState& state) {
    if (!g_loaded.load(std::memory_order_acquire) || !g_enabled.load(std::memory_order_relaxed) || g_failSafe.load(std::memory_order_relaxed)) {
        return false;
    }
    const auto found = g_byHash->find(draw.hash);
    if (found == g_byHash->end()) return false;
    bool skip = false;  // An instance in its object's first frame: drawn by no one.
    for (const Target& t : found->second) {
        Runtime* rt = t.rt;
        const auto components = rt->components.load(std::memory_order_acquire);
        const EfmiComponent& c = (*components)[t.component];
        if (c.indexCount != draw.indexCount) continue;
        rt->lastDrawFrame.store(g_stats.presents.load(std::memory_order_relaxed), std::memory_order_relaxed);

        Load load = rt->load.load(std::memory_order_acquire);
        if (load == Load::kNone && rt->load.compare_exchange_strong(load, Load::kLoading)) {
            PostJob([rt] { Guard("LoadGpu", [&] { LoadGpu(rt); }); });
        }
        if (load != Load::kReady) return false;
        if (draw.instanceCount != 1) {
            LogOnce(rt, t.component, kInstanced, "drawn as %u instances, which needs spatial identification (milestone 2d); the game's mesh is drawn",
                    draw.instanceCount);
            return false;
        }
        const EfmiDrawList& list = c.list;

        // Merged skeleton: every draw of a component gives its bones to the skeleton; and a
        // list recording its bones (cross-IB) gives them to the records.
        bool crossDraws = false;
        for (const EfmiDrawCall& d : list.draws) crossDraws |= d.boneSource >= 0;
        efmibones::BoneBinding bones;
        const bool skeletal = rt->hasSkeleton && c.componentId >= 0 && (c.gpuPosed || c.readOnly);
        const bool wantBones = skeletal || (rt->recordCount && (!list.recordIds.empty() || crossDraws));
        const bool boneBinding = draw.firstInstance == 0 && state.sets && efmibones::FindBoneBinding(state.sets, &bones);
        // EFMI's spatial identification of this instance: skipped on its object's first frame,
        // the game's draw until identified, the mod's once identified.
        uint32_t slot = 0;
        const Override what = Identify(rt, c.componentId, boneBinding ? bones.config : nullptr, g_stats.presents.load(std::memory_order_relaxed), &slot);
        g_identified[static_cast<int>(what)].fetch_add(1, std::memory_order_relaxed);
        if (what == Override::kSkip) {
            skip = true;
            continue;
        }
        if (what == Override::kOriginal) {
            LogOnce(rt, t.component, kUnidentified, "an object not (yet) identified as the mod's: the game's draw, as EFMI's");
            continue;
        }
        // Bones are imported at the mod's draws only (CommandListMergedSkeleton_Apply).
        const bool haveBones = wantBones && boneBinding;
        if (haveBones && skeletal) efmibones::Import(cb, &rt->skeleton, static_cast<uint32_t>(c.componentId), c.lod, bones, slot, bones.config);
        if (haveBones) {
            for (const int id : list.recordIds) efmibones::Import(cb, &rt->records, static_cast<uint32_t>(id), 0, bones, slot, bones.config);
        }
        if (g_hideReplaced.load(std::memory_order_relaxed)) return true;
        // The game textures the mod's draw list names, its ini overrides, and RabbitFX's.
        std::map<uint32_t, int> replace;
        for (const EfmiTextureBind& tb : list.textures) {
            if (tb.texture < static_cast<int>(rt->textureIds.size()) && rt->textureHashes[tb.texture]) {
                replace[rt->textureHashes[tb.texture]] = rt->textureIds[tb.texture];
            }
        }
        for (const auto& [hash, texture] : rt->thisByHash) {
            if (texture < static_cast<int>(rt->textureIds.size())) replace.emplace(hash, rt->textureIds[texture]);
        }
        // RabbitFX: its patched DX11 shaders sample the mod's diffuse, lightmap and normal map
        // in place of the game's. The exporter names each after the game texture it stands for
        // (t=...), the one the draw binds in that role: replaced by that hash, as the rest.
        for (const int texture : list.rabbit) {
            if (texture >= 0 && texture < static_cast<int>(rt->textureIds.size()) && rt->textureHashes[texture]) {
                replace[rt->textureHashes[texture]] = rt->textureIds[texture];
            }
        }
        efmitex::BindOptions options;
        options.drawKey = reinterpret_cast<uint64_t>(rt) ^ (uint64_t(t.component) << 48);
        options.pipeline = state.pipeline;
        if (list.drawOriginal) {
            // The game's mesh, drawn by the layer with the mod's textures in its sets.
            const uint32_t swapped = state.sets ? efmitex::Bind(cb, state.sets, replace, nullptr, 0, &options) : 0;
            if (!swapped) return false;
            g_fns.CmdDrawIndexed(cb, draw.indexCount, draw.instanceCount, draw.firstIndex, draw.vertexOffset, draw.firstInstance);
            efmitex::Restore(cb, state.sets, swapped);
            LogOnce(rt, t.component, kTextures, "the game's mesh, with the mod's textures");
            return true;
        }
        const std::vector<uint16_t>* palette = nullptr;
        if (c.gpuPosed && !c.readOnly) {  // skip_skeleton_override: drawn with the game's bones.
            if (!rt->hasSkeleton) {
                LogOnce(rt, t.component, kGpuPosed, "gpu_posed, but the ini has no merged skeleton the layer can read; the game's mesh is drawn");
                return false;
            }
            if (!haveBones) {
                LogOnce(rt, t.component, kNoBones, "cannot find where the game's draw reads its bones; the game's mesh is drawn");
                return false;
            }
            const int vb2 = list.draws.empty() ? -1 : list.draws[0].vb[2];
            if (vb2 < 0 || rt->palettes[vb2].empty()) {
                if (!list.draws.empty()) {
                    LogOnce(rt, t.component, kGpuPosed, "no usable bone indices (vb2); the game's mesh is drawn");
                    return false;
                }
            } else {
                palette = &rt->palettes[vb2];
            }
        }
        if (list.draws.empty() && list.otherBonesDraws) {
            LogOnce(rt, t.component, kOtherBones, "its draws need bones from another component (milestone 2b); the game's mesh is drawn");
            return false;
        }
        if (list.draws.empty()) return true;  // The mod hides it.
        // A list of nothing but a dummy triangle is an exporter's way to hide a component
        // (Chen's original face): hidden even when its buffers do not fit the game's.
        uint64_t listIndices = 0;
        for (const EfmiDrawCall& d : list.draws) listIndices += d.indexCount;
        const bool dummy = listIndices <= 3;
        // Draws under "if vs == N" are drawn in every pass: which Vulkan passes those DX11
        // shaders are cannot be told (culling does not: Chen's face is drawn front-culled).
        const bool outline = false;
        {
            static std::mutex passMu;
            static std::set<std::tuple<const Runtime*, size_t, uint64_t>>* passes = new std::set<std::tuple<const Runtime*, size_t, uint64_t>>();
            bool first;
            {
                std::lock_guard lock(passMu);
                first = passes->size() < 400 && passes->insert({rt, t.component, state.pipeline}).second;
            }
            if (first) {
                LogInfo("%s [%s]: pass: cull %u, front face %u, %u colour targets, depth write %d", rt->name.c_str(), c.section.c_str(), state.cullMode,
                        state.frontFace, state.colorTargets, state.depthWrite);
            }
        }

        // Every binding the pipeline reads needs a mod buffer of the same stride, in each draw.
        uint32_t used = 0, strides[kMaxBindings]{};
        for (uint32_t b = 0; b < kMaxBindings; ++b) {
            uint32_t stride = state.pipelineStrides[b];
            if (state.dynamicStride && state.vb[b].strideGiven) stride = static_cast<uint32_t>(state.vb[b].stride);
            if (!stride || (state.keepGame & (1u << b))) continue;  // Not mesh data: the game's stays.
            strides[b] = stride;
            used |= 1u << b;
        }
        if (!used) {
            LogOnce(rt, t.component, kNoPipeline, "the game's pipeline is unknown; the game's mesh is drawn");
            return false;
        }
        // The mod buffer a draw binds at a binding: its own; vb0 where it has none and the game
        // binds the same data there as at binding 0 (older exports have no vb3, newer ones
        // set vb3 = VB0); a trimmed copy when the game reads a shorter stride.
        const auto resolve = [&](const EfmiDrawCall& d, uint32_t b, bool log) -> const GpuBuffer* {
            int vb = b < kEfmiVbSlots ? d.vb[b] : -1;
            if (vb < 0 && b > 0 && d.vb[0] >= 0 && state.vb[b].buffer && state.vb[b].buffer == state.vb[0].buffer &&
                state.vb[b].offset == state.vb[0].offset) {
                vb = d.vb[0];
            }
            if (vb < 0) {
                if (log) {
                    LogOnce(rt, t.component, kNoBuffer,
                            "the game's pipeline reads per-vertex binding %u (stride %u), the mod has no vb%u; the game's mesh is drawn", b, strides[b], b);
                }
                return nullptr;
            }
            if (rt->gpu[vb].stride == strides[b]) return &rt->gpu[vb];
            // Wider in the game, where what it reads ends inside the mod's: padded.
            const uint32_t extent = state.pipelineExtents ? state.pipelineExtents[b] : 0;
            // Or position + normal where the game also reads a tangent: computed (AddTangents).
            const bool pad = strides[b] > rt->gpu[vb].stride && extent &&
                             (extent <= rt->gpu[vb].stride || (rt->gpu[vb].stride == 24 && strides[b] == 40 && extent == 40));
            // Or the game's attributes packed back to back are the mod's stride (DX11's layout,
            // Laevatain's skirt: UV + colour in 12 bytes, read at 0 and 16 of 20): repacked.
            const AttributeLayout* layout = state.pipelineLayouts ? &state.pipelineLayouts[b] : nullptr;
            uint32_t packed = 0;
            for (uint8_t a = 0; layout && a < layout->count; ++a) packed += layout->spans[a].size;
            bool repack = !pad && layout && layout->count && strides[b] > rt->gpu[vb].stride && packed == rt->gpu[vb].stride;
            // A pass reading only some of those attributes (the same stride): the copy another
            // pass's full layout made, when it has every attribute this one reads.
            if (!pad && !repack && layout && layout->count && strides[b] > rt->gpu[vb].stride) {
                for (const Trimmed& tr : rt->trimmed) {
                    if (tr.source != vb || tr.stride != strides[b] || !tr.repack.count) continue;
                    bool covered = true;
                    for (uint8_t a = 0; a < layout->count && covered; ++a) {
                        bool has = false;
                        for (uint8_t q = 0; q < tr.repack.count; ++q) {
                            has |= tr.repack.spans[q].offset == layout->spans[a].offset && tr.repack.spans[q].size == layout->spans[a].size;
                        }
                        covered = has;
                    }
                    if (covered) {
                        layout = &tr.repack;
                        repack = true;
                        break;
                    }
                }
            }
            if ((pad || repack || strides[b] < rt->gpu[vb].stride) && strides[b] % 4 == 0 && rt->gpu[vb].mapped) {
                const GpuBuffer* trimmed = TrimmedOf(rt, vb, strides[b], repack ? layout : nullptr);
                if (!trimmed && log) {
                    LogOnce(rt, t.component, kTrimming, "vertex binding %u: the game reads stride %u, the mod has %u; a trimmed copy is being made", b,
                            strides[b], rt->gpu[vb].stride);
                }
                return trimmed;
            }
            if (log) {
                LogOnce(rt, t.component, kStride, "vertex binding %u has stride %u in the game (attributes read up to byte %u), %u in the mod; the game's mesh is drawn",
                        b, strides[b], state.pipelineExtents ? state.pipelineExtents[b] : 0, rt->gpu[vb].stride);
            }
            return nullptr;
        };
        const auto fits = [&](const EfmiDrawCall& d, bool log) {
            for (uint32_t b = 0; b < kMaxBindings; ++b) {
                if ((used & (1u << b)) && !resolve(d, b, log)) return false;
            }
            return true;
        };
        // The list's own-bone draws decide: when they do not fit, the game's mesh is drawn.
        bool anyOwn = false;
        for (const EfmiDrawCall& d : list.draws) {
            if (d.boneSource >= 0) continue;
            // A dummy (one triangle) that does not fit hides the game's part all the same (Chen's
            // original skirt).
            if (!fits(d, !dummy)) return dummy;
            anyOwn = true;
        }

        efmitex::BufferPatch patches[2];
        uint32_t patchCount = 0;
        if (palette) {
            if (!efmibones::Redirect(cb, &rt->skeleton, static_cast<uint32_t>(c.componentId), c.lod, palette, bones, slot, bones.config, patches)) {
                LogOnce(rt, t.component, kRingFull, "the bone ring is full this frame; the game's mesh is drawn");
                return false;
            }
            patchCount = 2;
        }

        // Binds a draw's buffers where they differ from what is bound.
        int boundIb = -1;
        VkBuffer boundVb[kMaxBindings]{};
        const auto bindBuffers = [&](const EfmiDrawCall& d) {
            if (d.ib != boundIb) {
                const GpuBuffer& ib = rt->gpu[d.ib];
                g_fns.CmdBindIndexBuffer(cb, ib.buffer, 0, ib.indexBytes == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
                boundIb = d.ib;
            }
            const VkDeviceSize zero = 0;
            for (uint32_t b = 0; b < kMaxBindings; ++b) {
                if (!(used & (1u << b))) continue;
                const GpuBuffer* resolved = resolve(d, b, false);
                if (!resolved || resolved->buffer == boundVb[b]) continue;
                const GpuBuffer& vb = *resolved;
                if (state.dynamicStride && g_fns.CmdBindVertexBuffers2) {
                    const VkDeviceSize stride = vb.stride;
                    g_fns.CmdBindVertexBuffers2(cb, b, 1, &vb.buffer, &zero, nullptr, &stride);
                } else {
                    g_fns.CmdBindVertexBuffers(cb, b, 1, &vb.buffer, &zero);
                }
                boundVb[b] = vb.buffer;
            }
        };
        {
            // Which mod component a draw key is, for the slot lines of the log.
            static std::mutex keyMu;
            static std::set<uint64_t>* keys = new std::set<uint64_t>();
            std::lock_guard lock(keyMu);
            if (keys->insert(options.drawKey).second) {
                LogInfo("slot: draw %016llx is %s [%s], RabbitFX textures %d %d %d", (unsigned long long)options.drawKey, rt->name.c_str(),
                        c.section.c_str(), list.rabbit[0], list.rabbit[1], list.rabbit[2]);
            }
        }
        const uint32_t swapped = state.sets && anyOwn ? efmitex::Bind(cb, state.sets, replace, patches, patchCount, &options) : 0;
        if (patchCount) {
            const uint32_t needed = (1u << patches[0].set) | (1u << patches[1].set);
            if ((swapped & needed) != needed) {
                // Without the merged skeleton's bones bound, the mod's mesh would tear: put things back.
                if (swapped) efmitex::Restore(cb, state.sets, swapped);
                if (boundIb < 0) return false;  // Nothing of the mod's bound yet.
                if (state.indexBuffer) g_fns.CmdBindIndexBuffer(cb, state.indexBuffer, state.indexOffset, state.indexType);
                for (uint32_t b = 0; b < kMaxBindings; ++b) {
                    if (!(used & (1u << b)) || !state.vb[b].buffer) continue;
                    const VertexBinding& v = state.vb[b];
                    if (v.strideGiven && g_fns.CmdBindVertexBuffers2) {
                        g_fns.CmdBindVertexBuffers2(cb, b, 1, &v.buffer, &v.offset, nullptr, &v.stride);
                    } else {
                        g_fns.CmdBindVertexBuffers(cb, b, 1, &v.buffer, &v.offset);
                    }
                }
                LogOnce(rt, t.component, kNoBones, "could not bind the merged skeleton's bones; the game's mesh is drawn");
                return false;
            }
            LogOnce(rt, t.component, kSkeleton, "posed with the merged skeleton (%zu bones)", palette->size());
        }
        const int hideDraw = g_hideDraw.load(std::memory_order_relaxed);
        for (const EfmiDrawCall& d : list.draws) {
            if (d.boneSource >= 0 || (outline && d.shaderFiltered)) continue;
            if (static_cast<int>(&d - list.draws.data()) == hideDraw) continue;
            bindBuffers(d);
            g_fns.CmdDrawIndexed(cb, d.indexCount, draw.instanceCount, d.firstIndex, d.baseVertex, draw.firstInstance);
        }
        if (swapped) {
            efmitex::Restore(cb, state.sets, swapped);
            LogOnce(rt, t.component, kTextures, "draws with its own textures");
        }
        // Draws on another component's recorded bones: the record's bones and this draw's
        // config, through the ring, one source at a time.
        if (crossDraws) {
            uint32_t drawn = 0, skipped = 0;
            for (size_t source = 0; source < rt->recordCount; ++source) {
                bool any = false;
                for (const EfmiDrawCall& d : list.draws) any |= d.boneSource == static_cast<int>(source) && fits(d, false);
                if (!any) continue;
                efmitex::BufferPatch recordPatches[2];
                uint32_t bound = 0;
                if (haveBones && efmibones::Redirect(cb, &rt->records, static_cast<uint32_t>(source), 0, &rt->recordPalettes[source], bones, slot, bones.config,
                                                     recordPatches)) {
                    bound = efmitex::Bind(cb, state.sets, replace, recordPatches, 2, &options);
                    const uint32_t needed = (1u << recordPatches[0].set) | (1u << recordPatches[1].set);
                    if ((bound & needed) != needed) {
                        if (bound) efmitex::Restore(cb, state.sets, bound);
                        bound = 0;
                    }
                }
                for (const EfmiDrawCall& d : list.draws) {
                    if (d.boneSource != static_cast<int>(source) || (outline && d.shaderFiltered)) continue;
                    if (static_cast<int>(&d - list.draws.data()) == hideDraw) continue;
                    if (!bound || !fits(d, false)) {
                        ++skipped;
                        continue;
                    }
                    bindBuffers(d);
                    g_fns.CmdDrawIndexed(cb, d.indexCount, draw.instanceCount, d.firstIndex, d.baseVertex, draw.firstInstance);
                    ++drawn;
                }
                if (bound) efmitex::Restore(cb, state.sets, bound);
            }
            LogOnce(rt, t.component, kRecords, "%u draws on bones recorded by another component (cross-IB), %u left out", drawn, skipped);
        }
        // Put the game's bindings back: its later draws rely on them.
        if (state.indexBuffer) g_fns.CmdBindIndexBuffer(cb, state.indexBuffer, state.indexOffset, state.indexType);
        for (uint32_t b = 0; b < kMaxBindings; ++b) {
            if (!(used & (1u << b)) || !state.vb[b].buffer) continue;
            const VertexBinding& v = state.vb[b];
            if (v.strideGiven && g_fns.CmdBindVertexBuffers2) {
                g_fns.CmdBindVertexBuffers2(cb, b, 1, &v.buffer, &v.offset, nullptr, &v.stride);
            } else {
                g_fns.CmdBindVertexBuffers(cb, b, 1, &v.buffer, &v.offset);
            }
        }
        if (list.otherBonesDraws) {
            LogOnce(rt, t.component, kReplaced, "drawn by the mod (%zu draws; %u left out, they need bones from another component: milestone 2b)",
                    list.draws.size(), list.otherBonesDraws);
        } else {
            LogOnce(rt, t.component, kReplaced, "drawn by the mod (%zu draws)", list.draws.size());
        }
        return true;
    }
    return skip;
}

void OnPresent() {}

void LogStats() {
    LogInfo("spatial identification: %llu instances skipped (their object's first frame), %llu left to the game (not identified), %llu drawn by the mod",
            (unsigned long long)g_identified[0].load(), (unsigned long long)g_identified[1].load(), (unsigned long long)g_identified[2].load());
}

void PollKeys() {
    if (!g_loaded.load(std::memory_order_acquire)) return;
    // Debug views, behind Alt as WWMI12's were: Alt+M magenta test, Alt+H hide replaced draws.
    static bool mWasDown = false, hWasDown = false, nWasDown = false;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool m = alt && (GetAsyncKeyState('M') & 0x8000) != 0, h = alt && (GetAsyncKeyState('H') & 0x8000) != 0;
    if (m && !mWasDown) {
        const bool on = efmitex::ToggleMagenta();
        LogInfo("magenta test %s: slots given mod textures show magenta", on ? "on" : "off");
        MessageBeep(on ? MB_OK : MB_ICONHAND);
    }
    if (h && !hWasDown) {
        const bool on = !g_hideReplaced.load();
        g_hideReplaced.store(on);
        LogInfo("hide replaced draws %s: what is left is drawn by the game itself", on ? "on" : "off");
        MessageBeep(on ? MB_OK : MB_ICONHAND);
    }
    mWasDown = m;
    hWasDown = h;
    const bool n = alt && (GetAsyncKeyState('N') & 0x8000) != 0;
    if (n && !nWasDown) {
        // None, then the 1st, 2nd, ... draw of every list, up to the 16th, then none again.
        const int next = g_hideDraw.load() + 1 >= 16 ? -1 : g_hideDraw.load() + 1;
        g_hideDraw.store(next);
        if (next < 0) {
            LogInfo("hide draw: none");
        } else {
            LogInfo("hide draw: number %d of every mod draw list", next + 1);
        }
        MessageBeep(next < 0 ? MB_ICONHAND : MB_OK);
    }
    nWasDown = n;
    if (alt) return;  // Alt+key is a debug view, not a mod's key.
    for (Runtime* rt : *g_runtimes) {
        for (size_t k = 0; k < rt->keys.size(); ++k) {
            const bool down = (GetAsyncKeyState(rt->keys[k].vk) & 0x8000) != 0;
            if (down && !rt->keyDown[k]) Press(rt, k);
            rt->keyDown[k] = down;
        }
    }
}

bool ToggleEnabled() {
    const bool on = !g_enabled.load();
    g_enabled.store(on);
    return on;
}

}  // namespace efmivulkan::efmidraw
