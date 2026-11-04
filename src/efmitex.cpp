#include "efmitex.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <set>
#include <unordered_set>

#include "common.h"
#include "log.h"
#include "texhash.h"

namespace efmivulkan::efmitex {
namespace {

// A pool is reusable after this many presents. Some command buffers are submitted up to 3
// frames after they were recorded, and then run on the GPU for a frame or two more.
constexpr uint64_t kFramesInFlight = 8;
constexpr uint32_t kPoolSets = 2048, kPoolDescriptors = 16384;

DeviceFns g_fns;
Lookups g_lookups;

enum class State { kPending, kRecorded, kSubmitted, kReady };

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    std::vector<VkBufferImageCopy> regions;
    uint32_t mips = 1;
    uint32_t format = 0, width = 0, height = 0;
    State state = State::kPending;
    VkEvent done = VK_NULL_HANDLE;  // Set by the GPU after the copies. Never destroyed: few, and may still be pending.
};

// In place: a game texture a mod replaces by hash, of the same size and a copy-compatible
// format, gets the mod's pixels copied into it, at the start of one of the game's command
// buffers (as readbacks): from then on it holds them in every pass and draw, as cheeky-imp
// does at upload. When the game uploads into it again (streaming), it is copied again.
enum class InPlace { kQueued, kRecorded, kSubmitted, kDone };
struct InPlaceEntry {
    ImageDesc image;
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    int texture = -1;
    uint32_t levels = 1;
    InPlace state = InPlace::kQueued;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkEvent done = VK_NULL_HANDLE;
    uint64_t lastBound = 0;
    bool destroyed = false;
    bool redo = false;  // The game uploaded into it while our copy was in flight.
};
std::unordered_map<uint64_t, InPlaceEntry>* g_inPlace = new std::unordered_map<uint64_t, InPlaceEntry>();
std::atomic<bool> g_anyInPlace{false};
std::atomic<uint64_t> g_inPlaceDone{0}, g_inPlaceAgain{0};
constexpr uint32_t kInPlacePerBegin = 8;

std::mutex g_mu;  // Textures, pending uploads, pools.
std::vector<Texture*>* g_textures = new std::vector<Texture*>();
std::vector<int>* g_pending = new std::vector<int>();
std::unordered_map<VkCommandBuffer, std::vector<int>>* g_recorded = new std::unordered_map<VkCommandBuffer, std::vector<int>>();
std::atomic<uint64_t> g_frame{0};
std::atomic<bool> g_anyPending{false};
std::atomic<bool> g_anyRecorded{false};  // g_recorded is not empty.

// Readbacks: by image id. Retried a few times, a while apart, when the hash matches nothing.
constexpr uint32_t kReadbackTries = 4;
constexpr uint64_t kReadbackRetryFrames = 300;
constexpr uint32_t kReadbacksPerBegin = 4;
enum class Readback { kQueued, kRecorded, kSubmitted, kWaiting, kGaveUp };
struct ReadbackEntry {
    ImageDesc image;
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    Readback state = Readback::kQueued;
    uint32_t tries = 0;
    uint64_t retryFrame = 0;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const uint8_t* mapped = nullptr;
    uint64_t bytes = 0;
    VkEvent done = VK_NULL_HANDLE;
    uint64_t lastBound = 0;  // Frame a mod draw last bound it: copied only while that is recent.
    bool destroyed = false;  // By the game, while a copy was in flight: freed when it lands.
};
// Set while readbacks are being recorded: still set at the next begin means a fault cut the
// recording short, and readback is switched off rather than risk it again.
bool g_recordingReadbacks = false;
bool g_readbackOff = false;
// A copy goes only into a frame where the game uses the texture, so it cannot outlive it.
constexpr uint64_t kReadbackRecentFrames = 1;
std::unordered_map<uint64_t, ReadbackEntry>* g_readbacks = new std::unordered_map<uint64_t, ReadbackEntry>();
std::atomic<bool> g_anyReadback{false};
std::atomic<uint64_t> g_readbacksDone{0}, g_readbacksFailed{0};

// Two rings of pools: for set layouts with and without UPDATE_AFTER_BIND.
// Each frame's grows by another pool when one runs out: a set that cannot be had leaves the
// game's textures (and bones) on a mod draw for that frame, a flicker.
constexpr size_t kMaxPoolsPerFrame = 16;
struct PoolChain {
    std::vector<VkDescriptorPool> pools;
    size_t current = 0;
};
PoolChain g_pools[2][kFramesInFlight];
std::atomic<uint64_t> g_setsAllocated{0}, g_setsFailed{0};
std::atomic<uint64_t> g_stickyUsed{0};
std::atomic<uint64_t> g_inPlaceMipsShort{0};  // In-place copies not made: the mod's texture has fewer mips.
// Copies of game sets checked at submit, redone, and updated but not update-after-bind.
std::atomic<uint64_t> g_copiesRedone{0}, g_copiesStaleNotUab{0}, g_copiesChecked{0};
// Slots that had a mod texture last frame and the game's this frame (textures popping), by why:
// 0 the view's texture not hashed (and no earlier swap of that format), 1 hashed but not a
// mod's, 2 the mod's texture not ready.
std::atomic<uint64_t> g_dropped[3]{};
std::atomic<uint64_t> g_slotsSwapped{0};
std::atomic<int> g_magenta{-1};          // A 4x4 magenta texture, made on first use.
std::atomic<bool> g_showMagenta{false};

// ---- DDS files

struct Dds {
    uint32_t width = 0, height = 0, mips = 1, vkFormat = 0;
    size_t dataOffset = 0;
    std::vector<uint8_t> file;
};

uint32_t VkFormatOfDxgi(uint32_t dxgi) {
    switch (dxgi) {
        case 70: case 71: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case 72: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case 73: case 74: return VK_FORMAT_BC2_UNORM_BLOCK;
        case 75: return VK_FORMAT_BC2_SRGB_BLOCK;
        case 76: case 77: return VK_FORMAT_BC3_UNORM_BLOCK;
        case 78: return VK_FORMAT_BC3_SRGB_BLOCK;
        case 79: case 80: return VK_FORMAT_BC4_UNORM_BLOCK;
        case 81: return VK_FORMAT_BC4_SNORM_BLOCK;
        case 82: case 83: return VK_FORMAT_BC5_UNORM_BLOCK;
        case 84: return VK_FORMAT_BC5_SNORM_BLOCK;
        case 94: case 95: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case 96: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
        case 97: case 98: return VK_FORMAT_BC7_UNORM_BLOCK;
        case 99: return VK_FORMAT_BC7_SRGB_BLOCK;
        case 27: case 28: return VK_FORMAT_R8G8B8A8_UNORM;
        case 29: return VK_FORMAT_R8G8B8A8_SRGB;
        case 87: case 90: return VK_FORMAT_B8G8R8A8_UNORM;
        case 91: return VK_FORMAT_B8G8R8A8_SRGB;
        case 61: return VK_FORMAT_R8_UNORM;
        case 49: return VK_FORMAT_R8G8_UNORM;
        default: return 0;
    }
}

bool ReadDds(const std::wstring& path, Dds* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->file.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    const std::vector<uint8_t>& d = out->file;
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0) return false;
    const auto u32 = [&](size_t at) {
        uint32_t v;
        memcpy(&v, d.data() + at, 4);
        return v;
    };
    out->height = u32(12);
    out->width = u32(16);
    out->mips = std::max(1u, u32(28));
    const uint32_t fourCc = u32(84);
    out->dataOffset = 128;
    if (fourCc == 0x30315844) {  // "DX10"
        if (d.size() < 148) return false;
        out->vkFormat = VkFormatOfDxgi(u32(128));
        out->dataOffset = 148;
    } else if (fourCc == 0x31545844) {  // "DXT1"
        out->vkFormat = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    } else if (fourCc == 0x33545844) {  // "DXT3"
        out->vkFormat = VK_FORMAT_BC2_UNORM_BLOCK;
    } else if (fourCc == 0x35545844) {  // "DXT5"
        out->vkFormat = VK_FORMAT_BC3_UNORM_BLOCK;
    } else if (fourCc == 0x31495441 || fourCc == 0x55344342) {  // "ATI1", "BC4U"
        out->vkFormat = VK_FORMAT_BC4_UNORM_BLOCK;
    } else if (fourCc == 0x32495441 || fourCc == 0x55354342) {  // "ATI2", "BC5U"
        out->vkFormat = VK_FORMAT_BC5_UNORM_BLOCK;
    }
    return out->vkFormat != 0;
}

int MemoryType(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_fns.memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (g_fns.memory.memoryTypes[i].propertyFlags & want) == want) return static_cast<int>(i);
    }
    return -1;
}

bool Allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags want, VkDeviceMemory* memory) {
    const int type = MemoryType(req.memoryTypeBits, want);
    if (type < 0) return false;
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = req.size;
    info.memoryTypeIndex = static_cast<uint32_t>(type);
    return g_fns.AllocateMemory(g_fns.device, &info, nullptr, memory) == VK_SUCCESS;
}

// An image and view in VRAM, and a staging buffer with every mip, ready to record.
Texture* Create(const std::wstring& path) {
    Dds dds;
    if (!ReadDds(path, &dds)) {
        LogError("texture not usable (format not supported): %s", ToUtf8(path).c_str());
        return nullptr;
    }
    auto* t = new Texture();
    t->mips = dds.mips;
    t->format = dds.vkFormat;
    t->width = dds.width;
    t->height = dds.height;
    VkDeviceSize total = 0;
    for (uint32_t m = 0; m < dds.mips; ++m) {
        const uint32_t w = std::max(1u, dds.width >> m), h = std::max(1u, dds.height >> m);
        VkBufferImageCopy r{};
        r.bufferOffset = total;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        r.imageExtent = {w, h, 1};
        t->regions.push_back(r);
        total += texhash::MipBytes(dds.vkFormat, w, h);
    }
    if (dds.dataOffset + total > dds.file.size()) {
        LogError("texture file too short: %s", ToUtf8(path).c_str());
        return nullptr;
    }

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = static_cast<VkFormat>(dds.vkFormat);
    ii.extent = {dds.width, dds.height, 1};
    ii.mipLevels = dds.mips;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;  // SRC: copied in place.
    VkMemoryRequirements req;
    VkDeviceMemory imageMemory, stagingMemory;
    void* mapped = nullptr;
    if (g_fns.CreateImage(g_fns.device, &ii, nullptr, &t->image) != VK_SUCCESS) return nullptr;
    g_fns.GetImageMemoryRequirements(g_fns.device, t->image, &req);
    if (!Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &imageMemory) ||
        g_fns.BindImageMemory(g_fns.device, t->image, imageMemory, 0) != VK_SUCCESS) {
        return nullptr;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, dds.mips, 0, 1};
    if (g_fns.CreateImageView(g_fns.device, &vi, nullptr, &t->view) != VK_SUCCESS) return nullptr;

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = total;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (g_fns.CreateBuffer(g_fns.device, &bi, nullptr, &t->staging) != VK_SUCCESS) return nullptr;
    g_fns.GetBufferMemoryRequirements(g_fns.device, t->staging, &req);
    if (!Allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &stagingMemory) ||
        g_fns.BindBufferMemory(g_fns.device, t->staging, stagingMemory, 0) != VK_SUCCESS ||
        g_fns.MapMemory(g_fns.device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        return nullptr;
    }
    memcpy(mapped, dds.file.data() + dds.dataOffset, static_cast<size_t>(total));
    return t;
}

VkDescriptorPool NewPool(bool updateAfterBind) {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    static const VkDescriptorType kTypes[] = {
        VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT};
    VkDescriptorPoolSize sizes[std::size(kTypes)];
    for (size_t i = 0; i < std::size(kTypes); ++i) sizes[i] = {kTypes[i], kPoolDescriptors};
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.flags = updateAfterBind ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0;
    info.maxSets = kPoolSets;
    info.poolSizeCount = static_cast<uint32_t>(std::size(sizes));
    info.pPoolSizes = sizes;
    if (g_fns.CreateDescriptorPool(g_fns.device, &info, nullptr, &pool) != VK_SUCCESS) {
        LogError("cannot create a descriptor pool for mod textures");
        pool = VK_NULL_HANDLE;
    }
    return pool;
}

// A set of ours for this frame, from its chain of pools. g_mu held.
VkDescriptorSet AllocateSet(VkDescriptorSetLayout layout, bool updateAfterBind) {
    PoolChain& chain = g_pools[updateAfterBind][g_frame.load() % kFramesInFlight];
    VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    while (chain.current < kMaxPoolsPerFrame) {
        if (chain.current == chain.pools.size()) {
            const VkDescriptorPool pool = NewPool(updateAfterBind);
            if (!pool) break;
            chain.pools.push_back(pool);
        }
        info.descriptorPool = chain.pools[chain.current];
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult r = g_fns.AllocateDescriptorSets(g_fns.device, &info, &set);
        if (r == VK_SUCCESS) {
            g_setsAllocated.fetch_add(1, std::memory_order_relaxed);
            return set;
        }
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) break;
        ++chain.current;  // This one is full: the next.
    }
    g_setsFailed.fetch_add(1, std::memory_order_relaxed);
    return VK_NULL_HANDLE;
}

}  // namespace

void SetDevice(const DeviceFns& fns) { g_fns = fns; }
void SetLookups(const Lookups& l) { g_lookups = l; }

std::vector<int> Load(const std::string& mod, const std::vector<std::wstring>& files) {
    std::vector<int> ids;
    size_t loaded = 0;
    for (const std::wstring& file : files) {
        Texture* t = Create(file);
        if (!t) {
            ids.push_back(-1);
            continue;
        }
        std::lock_guard lock(g_mu);
        ids.push_back(static_cast<int>(g_textures->size()));
        g_pending->push_back(ids.back());
        g_textures->push_back(t);
        g_anyPending.store(true);
        ++loaded;
    }
    if (!files.empty()) LogInfo("%s: %zu of %zu textures loaded, uploading", mod.c_str(), loaded, files.size());
    return ids;
}

bool ToggleMagenta() {
    if (g_magenta.load() < 0) {
        // A DDS of 4x4 R8G8B8A8_UNORM_SRGB magenta, loaded as a mod texture is.
        const std::wstring path = LogDir() + L"\\magenta.dds";
        uint32_t header[37] = {0x20534444, 124, 0x1007, 4, 4, 16, 0, 1};
        header[19] = 32;
        header[20] = 4;
        header[21] = 0x30315844;  // "DX10"
        header[32] = 29;          // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        header[33] = 3;
        header[35] = 1;
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(header), sizeof(header));
        for (int i = 0; i < 16; ++i) {
            const uint8_t magenta[4] = {255, 0, 255, 255};
            f.write(reinterpret_cast<const char*>(magenta), 4);
        }
        f.close();
        const std::vector<int> ids = Load("magenta test", {path});
        if (!ids.empty()) g_magenta.store(ids[0]);
    }
    const bool on = !g_showMagenta.load();
    g_showMagenta.store(on);
    return on;
}

bool Ready(int id) {
    std::lock_guard lock(g_mu);
    if (id < 0 || id >= static_cast<int>(g_textures->size())) return false;
    Texture* t = (*g_textures)[id];
    if (t->state == State::kSubmitted && g_fns.GetEventStatus(g_fns.device, t->done) == VK_EVENT_SET) t->state = State::kReady;
    return t->state == State::kReady;
}

VkImageView View(int id) {
    std::lock_guard lock(g_mu);
    return id >= 0 && id < static_cast<int>(g_textures->size()) ? (*g_textures)[id]->view : VK_NULL_HANDLE;
}

// Memory the CPU reads fast: cached if there is any.
int ReadbackMemoryType(uint32_t bits) {
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const int cached = MemoryType(bits, host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    return cached >= 0 ? cached : MemoryType(bits, host);
}

// Readbacks queued, recorded into this command buffer: g_mu held.
void RecordReadbacks(VkCommandBuffer cb) {
    const uint64_t frame = g_frame.load();
    uint32_t recorded = 0;
    for (auto& [id, r] : *g_readbacks) {
        if (recorded == kReadbacksPerBegin) break;
        if (r.cb == cb && r.state == Readback::kRecorded) r.state = Readback::kQueued;  // Begun again unsubmitted.
        if (r.state == Readback::kWaiting && frame >= r.retryFrame) {
            g_fns.ResetVkEvent(g_fns.device, r.done);  // Set by the last try; not pending now.
            r.state = Readback::kQueued;
        }
        if (r.state != Readback::kQueued || r.destroyed || frame > r.lastBound + kReadbackRecentFrames) continue;
        if (!r.buffer) {
            // Every level: the lower ones tell streamed copies of this texture.
            r.bytes = 0;
            for (uint32_t m = 0; m < std::max(1u, r.image.mips); ++m) {
                r.bytes += texhash::MipBytes(r.image.format, std::max(1u, r.image.width >> m), std::max(1u, r.image.height >> m));
            }
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = r.bytes;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VkMemoryRequirements req;
            void* mapped = nullptr;
            VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
            if (!r.bytes || g_fns.CreateBuffer(g_fns.device, &bi, nullptr, &r.buffer) != VK_SUCCESS) {
                r.state = Readback::kGaveUp;
                continue;
            }
            g_fns.GetBufferMemoryRequirements(g_fns.device, r.buffer, &req);
            const int type = ReadbackMemoryType(req.memoryTypeBits);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = static_cast<uint32_t>(type);
            if (type < 0 || g_fns.AllocateMemory(g_fns.device, &ai, nullptr, &r.memory) != VK_SUCCESS ||
                g_fns.BindBufferMemory(g_fns.device, r.buffer, r.memory, 0) != VK_SUCCESS ||
                g_fns.MapMemory(g_fns.device, r.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS ||
                g_fns.CreateVkEvent(g_fns.device, &ei, nullptr, &r.done) != VK_SUCCESS) {
                r.state = Readback::kGaveUp;
                continue;
            }
            r.mapped = static_cast<const uint8_t*>(mapped);
        }
        // From the layout the game samples it in, to a copy, and back; after whatever wrote it.
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = r.image.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, std::max(1u, r.image.mips), 0, 1};
        b.oldLayout = r.layout;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        g_fns.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        std::vector<VkBufferImageCopy> regions(std::max(1u, r.image.mips));
        VkDeviceSize at = 0;
        for (uint32_t m = 0; m < regions.size(); ++m) {
            const uint32_t w = std::max(1u, r.image.width >> m), h = std::max(1u, r.image.height >> m);
            regions[m] = {};
            regions[m].bufferOffset = at;
            regions[m].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
            regions[m].imageExtent = {w, h, 1};
            at += texhash::MipBytes(r.image.format, w, h);
        }
        g_fns.CmdCopyImageToBuffer(cb, r.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.buffer, static_cast<uint32_t>(regions.size()),
                                   regions.data());
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = r.layout;
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.buffer = r.buffer;
        toHost.size = VK_WHOLE_SIZE;
        g_fns.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                                 &toHost, 1, &b);
        g_fns.CmdSetEvent(cb, r.done, VK_PIPELINE_STAGE_TRANSFER_BIT);
        r.state = Readback::kRecorded;
        r.cb = cb;
        ++recorded;
    }
}

// Readbacks the GPU has done: hashed, kept for a retry or let go. g_mu held.
void PollReadbacks(uint64_t frame) {
    std::vector<ReadbackEntry*> finished;
    for (auto& [id, r] : *g_readbacks) {
        if (r.state == Readback::kSubmitted && g_fns.GetEventStatus(g_fns.device, r.done) == VK_EVENT_SET) finished.push_back(&r);
    }
    for (ReadbackEntry* r : finished) {
        if (r->destroyed) {
            g_fns.DestroyBuffer(g_fns.device, r->buffer, nullptr);
            g_fns.FreeMemory(g_fns.device, r->memory, nullptr);
            g_readbacks->erase(r->image.id);  // r is gone after this.
            continue;
        }
        ++r->tries;
        g_readbacksDone.fetch_add(1, std::memory_order_relaxed);
        if (g_lookups.OnReadback) g_lookups.OnReadback(r->image, r->mapped, r->bytes);
        if (g_lookups.OnReadbackMip) {
            uint64_t at = texhash::MipBytes(r->image.format, r->image.width, r->image.height);
            for (uint32_t m = 1; m < r->image.mips && at < r->bytes; ++m) {
                const uint32_t w = std::max(1u, r->image.width >> m), h = std::max(1u, r->image.height >> m);
                const uint64_t size = texhash::MipBytes(r->image.format, w, h);
                g_lookups.OnReadbackMip(r->image, m, r->mapped + at, size);
                at += size;
            }
        }
        const bool matched = g_lookups.TextureHashOfImage && g_lookups.TextureHashOfImage(r->image.id) != 0;
        if (matched || r->tries >= kReadbackTries) {
            r->state = Readback::kGaveUp;  // Done with it, either way: its memory goes.
            if (!matched) g_readbacksFailed.fetch_add(1, std::memory_order_relaxed);
            g_fns.DestroyBuffer(g_fns.device, r->buffer, nullptr);
            g_fns.FreeMemory(g_fns.device, r->memory, nullptr);
            r->buffer = VK_NULL_HANDLE;
            r->memory = VK_NULL_HANDLE;
            r->mapped = nullptr;
        } else {
            r->state = Readback::kWaiting;
            r->retryFrame = frame + kReadbackRetryFrames;
        }
    }
}

// In-place copies the GPU has done. g_mu held.
void PollInPlace() {
    for (auto it = g_inPlace->begin(); it != g_inPlace->end();) {
        InPlaceEntry& e = it->second;
        if (e.state == InPlace::kSubmitted && g_fns.GetEventStatus(g_fns.device, e.done) == VK_EVENT_SET) {
            e.state = e.redo ? InPlace::kQueued : InPlace::kDone;
            e.redo = false;
            g_inPlaceDone.fetch_add(1, std::memory_order_relaxed);
            if (e.destroyed) {
                it = g_inPlace->erase(it);
                continue;
            }
        }
        ++it;
    }
}

// Queued in-place copies, recorded into this command buffer. g_mu held.
void RecordInPlace(VkCommandBuffer cb) {
    const uint64_t frame = g_frame.load();
    uint32_t recorded = 0;
    for (auto& [id, e] : *g_inPlace) {
        if (recorded == kInPlacePerBegin) break;
        if (e.cb == cb && e.state == InPlace::kRecorded) e.state = InPlace::kQueued;  // Begun again unsubmitted.
        if (e.state != InPlace::kQueued || e.destroyed || frame > e.lastBound + kReadbackRecentFrames) continue;
        Texture* t = (*g_textures)[e.texture];
        if (t->state != State::kReady) continue;
        if (!e.done) {
            VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
            if (g_fns.CreateVkEvent(g_fns.device, &ei, nullptr, &e.done) != VK_SUCCESS) continue;
        } else {
            g_fns.ResetVkEvent(g_fns.device, e.done);  // Set by the last copy; not pending now.
        }
        VkImageMemoryBarrier b[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
        for (VkImageMemoryBarrier& x : b) x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[0].image = e.image.image;
        b[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, e.levels, 0, 1};
        b[0].oldLayout = e.layout;
        b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b[1].image = t->image;
        b[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t->mips, 0, 1};
        b[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        g_fns.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
        std::vector<VkImageCopy> copies(e.levels);
        for (uint32_t l = 0; l < e.levels; ++l) {
            VkImageCopy& c = copies[l];
            c = {};
            c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
            c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
            c.extent = {std::max(1u, t->width >> l), std::max(1u, t->height >> l), 1};
        }
        g_fns.CmdCopyImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, e.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, e.levels,
                           copies.data());
        b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[0].newLayout = e.layout;
        b[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b[1].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        g_fns.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
        g_fns.CmdSetEvent(cb, e.done, VK_PIPELINE_STAGE_TRANSFER_BIT);
        e.state = InPlace::kRecorded;
        e.cb = cb;
        ++recorded;
    }
}

void OnBegin(VkCommandBuffer cb) {
    if (!g_anyPending.load(std::memory_order_relaxed) && !g_anyRecorded.load(std::memory_order_relaxed) &&
        !g_anyReadback.load(std::memory_order_relaxed) && !g_anyInPlace.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard lock(g_mu);
    if (g_recordingReadbacks && !g_readbackOff) {
        g_readbackOff = true;
        LogError("textures: a fault while recording a readback; readback is off for this session");
    }
    if (g_anyReadback.load(std::memory_order_relaxed)) {
        PollReadbacks(g_frame.load());
        if (!g_readbackOff) {
            g_recordingReadbacks = true;
            RecordReadbacks(cb);
            g_recordingReadbacks = false;
        }
    }
    if (g_anyInPlace.load(std::memory_order_relaxed)) {
        PollInPlace();
        if (!g_readbackOff) {
            g_recordingReadbacks = true;  // The same guard: a fault here switches both off.
            RecordInPlace(cb);
            g_recordingReadbacks = false;
        }
    }
    // Recorded into this command buffer before, but begun again without being submitted.
    const auto again = g_recorded->find(cb);
    if (again != g_recorded->end()) {
        for (int id : again->second) {
            (*g_textures)[id]->state = State::kPending;
            g_pending->push_back(id);
        }
        g_recorded->erase(again);
        g_anyRecorded.store(!g_recorded->empty());
    }
    if (g_pending->empty()) {
        g_anyPending.store(false);
        return;
    }
    VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VkEvent done;
    if (g_fns.CreateVkEvent(g_fns.device, &ei, nullptr, &done) != VK_SUCCESS) return;
    std::vector<VkImageMemoryBarrier> toCopy, toRead;
    for (int id : *g_pending) {
        const Texture* t = (*g_textures)[id];
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t->image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t->mips, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toCopy.push_back(b);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.push_back(b);
    }
    g_fns.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             static_cast<uint32_t>(toCopy.size()), toCopy.data());
    for (int id : *g_pending) {
        Texture* t = (*g_textures)[id];
        g_fns.CmdCopyBufferToImage(cb, t->staging, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(t->regions.size()),
                                   t->regions.data());
        t->state = State::kRecorded;
        t->done = done;
    }
    g_fns.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                             static_cast<uint32_t>(toRead.size()), toRead.data());
    g_fns.CmdSetEvent(cb, done, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    (*g_recorded)[cb] = std::move(*g_pending);
    g_pending->clear();
    g_anyPending.store(false);
    g_anyRecorded.store(true);
}

void SubmittedCopies(VkCommandBuffer cb);  // Below, with Bind.

void OnSubmitted(VkCommandBuffer cb) {
    SubmittedCopies(cb);
    if (!g_anyRecorded.load(std::memory_order_relaxed) && !g_anyReadback.load(std::memory_order_relaxed) &&
        !g_anyInPlace.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard lock(g_mu);
    for (auto& [id, e] : *g_inPlace) {
        if (e.cb == cb && e.state == InPlace::kRecorded) e.state = InPlace::kSubmitted;
    }
    for (auto& [id, r] : *g_readbacks) {
        if (r.cb == cb && r.state == Readback::kRecorded) r.state = Readback::kSubmitted;
    }
    const auto it = g_recorded->find(cb);
    if (it == g_recorded->end()) return;
    for (int id : it->second) (*g_textures)[id]->state = State::kSubmitted;
    g_recorded->erase(it);
    g_anyRecorded.store(!g_recorded->empty());
}

void OnPresent() {
    std::lock_guard lock(g_mu);
    const uint64_t frame = g_frame.fetch_add(1) + 1;
    PollReadbacks(frame);
    PollInPlace();
    // This frame's pools were last used kFramesInFlight frames ago: the GPU is done with them.
    for (auto& ring : g_pools) {
        PoolChain& chain = ring[frame % kFramesInFlight];
        for (VkDescriptorPool pool : chain.pools) g_fns.ResetDescriptorPool(g_fns.device, pool, 0);
        chain.current = 0;
    }
}

void RequestReadback(const ImageDesc& image, VkImageLayout layout) {
    std::lock_guard lock(g_mu);
    const uint64_t frame = g_frame.load();
    const auto it = g_readbacks->find(image.id);
    if (it != g_readbacks->end()) {
        it->second.lastBound = frame;
        it->second.layout = layout;
        return;
    }
    ReadbackEntry r;
    r.image = image;
    r.layout = layout;
    r.lastBound = frame;
    (*g_readbacks)[image.id] = r;
    g_anyReadback.store(true, std::memory_order_relaxed);
}

// Off: DX11 EFMI never writes into the game's textures (3DMigoto swaps them as each draw
// binds them), and copies here left the game's pixels in the smaller mips: the mod's texture
// and the game's popping in and out. Kept for an experiment, behind kInPlace.
constexpr bool kInPlace = false;

void RequestInPlace(const ImageDesc& image, VkImageLayout layout, int texture) {
    if (!kInPlace) return;
    std::lock_guard lock(g_mu);
    const uint64_t frame = g_frame.load();
    const auto it = g_inPlace->find(image.id);
    if (it != g_inPlace->end()) {
        it->second.lastBound = frame;
        it->second.layout = layout;
        return;
    }
    if (texture < 0 || texture >= static_cast<int>(g_textures->size())) return;
    const Texture* t = (*g_textures)[texture];
    // Same size and the very same format: a copy moves bytes, and the game's view reads them
    // in its own format. Mods ship "BC5" normal maps that are BC7 and "sRGB" colour that is
    // UNORM; copied into the game's texture those come out as garbage normals or wrong
    // colour, where the per-draw swap binds them with their own format, as DX11 EFMI does.
    if (!image.readable || t->width != image.width || t->height != image.height || t->format != image.format) return;
    // And every mip the game's has: most mod textures have one, and a copy of it alone left
    // the game's pixels in the smaller mips, sampled with distance and angle: the mod's
    // texture and the game's popping in and out. Those get the per-draw swap only.
    if (t->mips < image.mips) {
        g_inPlaceMipsShort.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    InPlaceEntry e;
    e.image = image;
    e.layout = layout;
    e.texture = texture;
    e.levels = std::max(1u, std::min(t->mips, image.mips));
    e.lastBound = frame;
    (*g_inPlace)[image.id] = e;
    g_anyInPlace.store(true, std::memory_order_relaxed);
}

void OnGameUpload(uint64_t imageId) {
    if (!g_anyInPlace.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_mu);
    const auto it = g_inPlace->find(imageId);
    if (it == g_inPlace->end() || it->second.state == InPlace::kQueued) return;
    // The game's pixels again: ours go over them again, once any copy in flight lands.
    if (it->second.state == InPlace::kDone) {
        it->second.state = InPlace::kQueued;
    } else {
        it->second.redo = true;
    }
    g_inPlaceAgain.fetch_add(1, std::memory_order_relaxed);
}

void OnImageDestroyed(uint64_t imageId) {
    if (!g_anyReadback.load(std::memory_order_relaxed) && !g_anyInPlace.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_mu);
    const auto ip = g_inPlace->find(imageId);
    if (ip != g_inPlace->end()) {
        if (ip->second.state == InPlace::kRecorded || ip->second.state == InPlace::kSubmitted) {
            ip->second.destroyed = true;
        } else {
            g_inPlace->erase(ip);
        }
    }
    const auto it = g_readbacks->find(imageId);
    if (it == g_readbacks->end()) return;
    ReadbackEntry& r = it->second;
    if (r.state == Readback::kRecorded || r.state == Readback::kSubmitted) {
        r.destroyed = true;  // Its copy is in flight: its memory goes when it lands.
        return;
    }
    if (r.buffer) {
        g_fns.DestroyBuffer(g_fns.device, r.buffer, nullptr);
        g_fns.FreeMemory(g_fns.device, r.memory, nullptr);
    }
    g_readbacks->erase(it);
}

void LogStats() {
    size_t queued = 0;
    {
        std::lock_guard lock(g_mu);
        for (const auto& [id, r] : *g_readbacks) queued += r.state != Readback::kGaveUp;
    }
    LogInfo("textures: %llu mod textures copied into the game's in place (%llu again after the game uploaded into them; %llu not, "
            "as the mod's has fewer mips)",
            (unsigned long long)g_inPlaceDone.load(), (unsigned long long)g_inPlaceAgain.load(), (unsigned long long)g_inPlaceMipsShort.load());
    LogInfo("descriptor sets of ours: %llu made, %llu could not be (that draw kept the game's textures); textures put in by an "
            "earlier swap in the same slot %llu",
            (unsigned long long)g_setsAllocated.load(), (unsigned long long)g_setsFailed.load(), (unsigned long long)g_stickyUsed.load());
    LogInfo("descriptor sets of ours checked at submit: %llu, %llu redone as their game set was updated after recording, %llu updated "
            "but not update-after-bind (left)",
            (unsigned long long)g_copiesChecked.load(), (unsigned long long)g_copiesRedone.load(), (unsigned long long)g_copiesStaleNotUab.load());
    if (!g_anyReadback.load()) return;
    LogInfo("textures: %llu read back from the GPU and hashed (%llu matched nothing after %u tries), %zu still to do",
            (unsigned long long)g_readbacksDone.load(), (unsigned long long)g_readbacksFailed.load(), kReadbackTries, queued);
}

namespace {

struct Sticky {
    int texture;
    uint32_t format;  // The game texture's it replaced.
};
std::unordered_map<uint64_t, Sticky>* g_sticky = new std::unordered_map<uint64_t, Sticky>();  // Under g_mu.

uint64_t StickyKey(const BindOptions& o, uint32_t set, uint32_t offset, uint32_t element) {
    uint64_t h = o.drawKey * 0x9E3779B97F4A7C15ull ^ o.pipeline;
    h = (h ^ (uint64_t(set) << 40 | uint64_t(offset) << 8 | element)) * 0xC2B2AE3D27D4EB4Full;
    return h ^ (h >> 31);
}

}  // namespace

// A set of ours, a copy of a game set with edits. Unity writes some sets after the draws
// using them are recorded (UPDATE_AFTER_BIND): the game's draws then read the new contents
// at submit, the copy the old ones (last frame's camera and object data: the mod jitters
// against the game). At submit each copy is redone from its game set if it changed.
struct Edit {
    uint32_t at, bytes;
    uint8_t value[24];
};
struct CopyOfSet {
    VkDescriptorSet game, ours;
    VkDescriptorUpdateTemplate tmpl;
    uint64_t updates;
    std::vector<Edit> edits;
};
std::mutex g_copyMu;
std::unordered_map<VkCommandBuffer, std::vector<CopyOfSet>>* g_copies = new std::unordered_map<VkCommandBuffer, std::vector<CopyOfSet>>();
std::unordered_map<VkCommandBuffer, std::vector<VkCommandBuffer>>* g_copyChildren = new std::unordered_map<VkCommandBuffer, std::vector<VkCommandBuffer>>();

void RedoCopies(std::vector<CopyOfSet>& copies) {
    SetContents now;
    for (CopyOfSet& c : copies) {
        g_copiesChecked.fetch_add(1, std::memory_order_relaxed);
        if (!g_lookups.Updates || g_lookups.Updates(c.game) == c.updates) continue;
        if (!g_lookups.Contents(c.game, &now) || now.tmpl != c.tmpl) continue;
        if (!now.updateAfterBind) {
            g_copiesStaleNotUab.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        for (const Edit& e : c.edits) {
            if (e.at + e.bytes <= now.data.size()) memcpy(now.data.data() + e.at, e.value, e.bytes);
        }
        g_fns.UpdateDescriptorSetWithTemplate(g_fns.device, c.ours, c.tmpl, now.data.data());
        g_copiesRedone.fetch_add(1, std::memory_order_relaxed);
    }
}

void OnExecute(VkCommandBuffer primary, uint32_t count, const VkCommandBuffer* secondaries) {
    std::lock_guard lock(g_copyMu);
    if (g_copies->empty()) return;
    auto& children = (*g_copyChildren)[primary];
    children.insert(children.end(), secondaries, secondaries + count);
}

void SubmittedCopies(VkCommandBuffer cb) {
    std::vector<CopyOfSet> copies;
    {
        std::lock_guard lock(g_copyMu);
        if (g_copies->empty()) return;
        const auto take = [&](VkCommandBuffer b) {
            const auto it = g_copies->find(b);
            if (it == g_copies->end()) return;
            copies.insert(copies.end(), std::make_move_iterator(it->second.begin()), std::make_move_iterator(it->second.end()));
            g_copies->erase(it);
        };
        take(cb);
        const auto ch = g_copyChildren->find(cb);
        if (ch != g_copyChildren->end()) {
            for (VkCommandBuffer child : ch->second) take(child);
            g_copyChildren->erase(ch);
        }
    }
    RedoCopies(copies);
}

uint32_t Bind(VkCommandBuffer cb, const BoundSet* sets, const std::map<uint32_t, int>& replace, const BufferPatch* patches,
              uint32_t patchCount, const BindOptions* options) {
    if ((replace.empty() && !patchCount) || !g_lookups.Contents) return 0;
    uint32_t bound = 0;
    SetContents c;
    for (uint32_t i = 0; i < kMaxSets; ++i) {
        bool patched = false;
        for (uint32_t p = 0; p < patchCount; ++p) patched |= patches[p].set == i;
        if (!sets[i].set || (!patched && replace.empty())) continue;
        if (!g_lookups.Contents(sets[i].set, &c) || (c.images.empty() && !patched)) continue;
        bool changed = false;
        uint32_t bufferEdits[8], bufferEditCount = 0;
        std::vector<Edit> edits;
        uint32_t dynamic[kMaxDynamic];
        memcpy(dynamic, sets[i].dynamic, sizeof(dynamic));
        for (uint32_t p = 0; p < patchCount; ++p) {
            const BufferPatch& patch = patches[p];
            if (patch.set != i) continue;
            for (const auto& e : c.entries) {
                if (!IsBuffer(e.descriptorType) || e.dstBinding != patch.binding || patch.element < e.dstArrayElement ||
                    patch.element >= e.dstArrayElement + e.descriptorCount) {
                    continue;
                }
                const size_t at = e.offset + size_t(patch.element - e.dstArrayElement) * e.stride;
                if (at + sizeof(VkDescriptorBufferInfo) > c.data.size()) continue;
                auto* info = reinterpret_cast<VkDescriptorBufferInfo*>(c.data.data() + at);
                bufferEdits[bufferEditCount++ % 8] = static_cast<uint32_t>(at);
                info->buffer = patch.buffer;
                info->offset = patch.offset;
                info->range = patch.range;
                if (IsDynamic(e.descriptorType)) {
                    const int d = DynamicIndex(c.entries, patch.binding, patch.element);
                    if (d < 0 || d >= static_cast<int>(sets[i].dynamicCount)) continue;
                    dynamic[d] = patch.dynamicOffset;
                } else {
                    info->offset += patch.dynamicOffset;  // Not dynamic: the block's place goes in the descriptor.
                }
                changed = true;
            }
        }
        for (const ImageEntry& e : c.images) {
            for (uint32_t k = 0; k < e.count; ++k) {
                const size_t at = size_t(e.offset) + size_t(k) * e.stride;
                if (at + sizeof(VkDescriptorImageInfo) > c.data.size()) break;
                auto* info = reinterpret_cast<VkDescriptorImageInfo*>(c.data.data() + at);
                if (!info->imageView) continue;
                const uint32_t hash = g_lookups.TextureHashOfView(info->imageView);
                ImageDesc image;
                bool described = false;
                const auto describe = [&] {
                    if (!described) described = g_lookups.ImageOfView && g_lookups.ImageOfView(info->imageView, &image);
                    return described;
                };
                int use = -1;
                if (hash) {
                    const auto r = replace.find(hash);
                    if (r != replace.end() && Ready(r->second)) {
                        use = r->second;
                        // Once a frame per thread and view: the game texture gets the mod's pixels.
                        thread_local std::unordered_map<VkImageView, uint64_t> inPlaceAsked;
                        const uint64_t frame = g_frame.load(std::memory_order_relaxed) + 1;
                        if (inPlaceAsked[info->imageView] != frame && (inPlaceAsked[info->imageView] = frame) && describe()) {
                            RequestInPlace(image, info->imageLayout, use);
                        }
                    }
                }
                if (!hash) {
                    // Each thread asks once per view a frame: mod draws bind the same ones every frame.
                    thread_local std::unordered_map<VkImageView, uint64_t> asked;  // View -> frame asked + 1.
                    const uint64_t frame = g_frame.load(std::memory_order_relaxed) + 1;
                    if (asked[info->imageView] != frame && (asked[info->imageView] = frame) && describe() && image.readable) {
                        RequestReadback(image, info->imageLayout);
                    }
                    if (use >= 0 && !Ready(use)) use = -1;
                    if (use >= 0) g_stickyUsed.fetch_add(1, std::memory_order_relaxed);
                }
                // Once per mod component and pass: each texture slot, and what went in.
                if (options && describe()) {
                    static std::mutex slotMu;
                    static std::set<uint64_t>* slotsLogged = new std::set<uint64_t>();
                    bool first;
                    {
                        std::lock_guard lock(slotMu);
                        first = slotsLogged->size() < 8000 &&
                                slotsLogged->insert(StickyKey(*options, i, e.offset, k) ^ (uint64_t(hash) << 20) ^ uint64_t(use + 1)).second;
                    }
                    if (first) {
                        LogInfo("slot: draw %016llx pass %016llx set %u at %u: %ux%u format %u, hash %08x -> %s%d", (unsigned long long)options->drawKey,
                                (unsigned long long)options->pipeline, i, e.offset + k * e.stride, image.width, image.height, image.format, hash,
                                use < 0 ? "game's " : "mod texture ", use);
                    }
                }
                if (use < 0) continue;
                if (g_showMagenta.load(std::memory_order_relaxed) && Ready(g_magenta.load())) use = g_magenta.load();
                info->imageView = View(use);
                info->imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                changed = true;
                if (c.updateAfterBind) {
                    // View and layout only: the sampler stays the game's.
                    Edit ed{static_cast<uint32_t>(at + offsetof(VkDescriptorImageInfo, imageView)), 16, {}};
                    memcpy(ed.value, c.data.data() + ed.at, 16);
                    edits.push_back(ed);
                }
            }
        }
        if (!changed) continue;
        VkDescriptorSet ours = VK_NULL_HANDLE;
        {
            std::lock_guard lock(g_mu);
            ours = AllocateSet(c.layout, c.updateAfterBind);
            if (!ours) continue;
            g_fns.UpdateDescriptorSetWithTemplate(g_fns.device, ours, c.tmpl, c.data.data());
        }
        if (c.updateAfterBind && g_lookups.Updates) {
            for (uint32_t k = 0; k < bufferEditCount && k < 8; ++k) {
                Edit ed{bufferEdits[k], sizeof(VkDescriptorBufferInfo), {}};
                memcpy(ed.value, c.data.data() + ed.at, sizeof(VkDescriptorBufferInfo));
                edits.push_back(ed);
            }
            std::lock_guard lock(g_copyMu);
            (*g_copies)[cb].push_back({sets[i].set, ours, c.tmpl, c.updates, std::move(edits)});
        }
        g_fns.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, sets[i].layout, i, 1, &ours, sets[i].dynamicCount, dynamic);
        bound |= 1u << i;
    }
    return bound;
}

void Restore(VkCommandBuffer cb, const BoundSet* sets, uint32_t bound) {
    for (uint32_t i = 0; i < kMaxSets; ++i) {
        if (!(bound & (1u << i))) continue;
        g_fns.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, sets[i].layout, i, 1, &sets[i].set, sets[i].dynamicCount, sets[i].dynamic);
    }
}

}  // namespace efmivulkan::efmitex
