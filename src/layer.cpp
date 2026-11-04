// The Vulkan layer: the loader puts it between Endfield and the driver, so every
// Vulkan call the game makes passes through here. Only the calls EFMI needs are
// intercepted; everything else goes straight to the next layer or the driver.
//
// What is tracked, and why:
//   buffers, memory, maps    which buffer is where, and which can be read by the CPU
//   vkCmdCopyBuffer          uploads: staging bytes copied into a buffer, read at submit
//   vkCmdBindIndexBuffer     the index buffer of each command buffer
//   pipelines, vertex bufs   the vertex bindings a draw reads, and their strides
//   images, image uploads    3DMigoto's texture hash of each upload's mip 0 (textures.cpp)
//   image views              the image each one shows, for its texture hash
//   descriptor templates     the data each set was last written with, and the sets each
//                            command buffer has bound, to swap textures (efmitex.cpp)
//   command pools            which command buffers can take our texture uploads
//   vkCmdDrawIndexed         EFMI's identification (efmi.cpp), then the mod's draw (efmidraw.cpp)
//   vkQueuePresentKHR        frames
//
// Hot path, from WWMI12: a draw costs one map lookup for its command buffer (cached per
// thread) and one set lookup for its index count. Nothing else runs unless a mod uses
// that index count.
#define VK_NO_PROTOTYPES
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <set>
#include <unordered_map>
#include <vector>

#include "common.h"
#include "efmi.h"
#include "efmidraw.h"
#include "efmibones.h"
#include "efmitex.h"
#include "ibregion.h"
#include "texhash.h"
#include "textures.h"
#include "log.h"

namespace efmivulkan {
namespace {

constexpr char kLayerName[] = "VK_LAYER_EFMIVULKAN_mod_loader";

// Only Endfield, or a test that asks for it: anywhere else the layer passes through.
bool Active() {
    static const bool active = [] {
        wchar_t value[8]{};
        if (GetEnvironmentVariableW(L"EFMIVULKAN_ANY_PROCESS", value, 8) && value[0] == L'1') return true;
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* name = wcsrchr(exe, L'\\');
        return _wcsicmp(name ? name + 1 : exe, L"Endfield.exe") == 0;
    }();
    return active;
}

// Dispatchable handles start with the loader's dispatch table pointer, shared by an
// instance and its physical devices, and by a device and its queues and command buffers.
void* DispatchKey(const void* handle) { return *static_cast<void* const*>(handle); }

struct InstanceData {
    VkInstance instance;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
};

struct DeviceData {
    void* dispatchKey;
    VkDevice device;
    PFN_vkGetDeviceProcAddr gdpa;
    VkPhysicalDeviceMemoryProperties memory;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkMapMemory2KHR MapMemory2;  // vkMapMemory2 (1.4) or vkMapMemory2KHR: the same.
    PFN_vkMapMemory2KHR MapMemory2KHR;
    PFN_vkUnmapMemory2KHR UnmapMemory2;
    PFN_vkUnmapMemory2KHR UnmapMemory2KHR;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkBindBufferMemory2 BindBufferMemory2;
    PFN_vkBindBufferMemory2 BindBufferMemory2KHR;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkCmdCopyBuffer CmdCopyBuffer;
    PFN_vkCmdCopyBuffer2 CmdCopyBuffer2;
    PFN_vkCmdCopyBuffer2 CmdCopyBuffer2KHR;
    PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer;
    PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers;
    PFN_vkCmdBindVertexBuffers2 CmdBindVertexBuffers2;
    PFN_vkCmdBindVertexBuffers2 CmdBindVertexBuffers2EXT;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
    PFN_vkDestroyPipeline DestroyPipeline;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkCmdDrawIndexed CmdDrawIndexed;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
    PFN_vkCmdCopyBufferToImage2 CmdCopyBufferToImage2;
    PFN_vkCmdCopyBufferToImage2 CmdCopyBufferToImage2KHR;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
    PFN_vkUpdateDescriptorSetWithTemplate UpdateDescriptorSetWithTemplate;
    PFN_vkUpdateDescriptorSetWithTemplate UpdateDescriptorSetWithTemplateKHR;
    PFN_vkCmdPushDescriptorSetKHR CmdPushDescriptorSetKHR;
    PFN_vkCmdPushDescriptorSetWithTemplateKHR CmdPushDescriptorSetWithTemplateKHR;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
    PFN_vkCmdDrawIndexedIndirect CmdDrawIndexedIndirect;
    PFN_vkCmdDrawIndexedIndirectCount CmdDrawIndexedIndirectCount;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkQueueSubmit2 QueueSubmit2;
    PFN_vkQueueSubmit2 QueueSubmit2KHR;
    PFN_vkQueuePresentKHR QueuePresentKHR;
    PFN_vkCmdExecuteCommands CmdExecuteCommands;
    PFN_vkCmdSetCullMode CmdSetCullMode;
    PFN_vkCmdSetCullMode CmdSetCullModeEXT;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCreateDescriptorPool CreateDescriptorPool;
    PFN_vkResetDescriptorPool ResetDescriptorPool;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkCreateDescriptorUpdateTemplate CreateDescriptorUpdateTemplate;
    PFN_vkCreateDescriptorUpdateTemplate CreateDescriptorUpdateTemplateKHR;
    PFN_vkDestroyDescriptorUpdateTemplate DestroyDescriptorUpdateTemplate;
    PFN_vkDestroyDescriptorUpdateTemplate DestroyDescriptorUpdateTemplateKHR;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkCreateEvent CreateVkEvent;
    PFN_vkCmdSetEvent CmdSetEvent;
    PFN_vkCreateBufferView CreateBufferView;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkCmdCopyImage CmdCopyImage;
    PFN_vkResetEvent ResetVkEvent;
    PFN_vkDestroyBufferView DestroyBufferView;
    PFN_vkGetEventStatus GetEventStatus;
    std::vector<VkQueueFlags> families;  // Of each queue family.
};

std::shared_mutex g_dispatchMu;
std::unordered_map<void*, InstanceData>* g_instances = new std::unordered_map<void*, InstanceData>();
std::unordered_map<void*, DeviceData*>* g_devices = new std::unordered_map<void*, DeviceData*>();

InstanceData* Instance(const void* handle) {
    std::shared_lock lock(g_dispatchMu);
    const auto it = g_instances->find(DispatchKey(handle));
    return it == g_instances->end() ? nullptr : &it->second;
}

DeviceData* Device(const void* handle) {
    // One device in practice: remember the last. DeviceData is never freed, and holds
    // its own key, so a single pointer is a consistent cache.
    static std::atomic<DeviceData*> last{nullptr};
    void* key = DispatchKey(handle);
    DeviceData* cached = last.load(std::memory_order_acquire);
    if (cached && cached->dispatchKey == key) return cached;
    std::shared_lock lock(g_dispatchMu);
    const auto it = g_devices->find(key);
    if (it == g_devices->end()) return nullptr;
    last.store(it->second, std::memory_order_release);
    return it->second;
}

// ---- Buffers and memory

struct MemoryInfo {
    VkDeviceSize size = 0;
    bool hostVisible = false;
    uint8_t* mappedBase = nullptr;  // Pointer to offset 0 while mapped.
};
struct BufferInfo {
    uint32_t id = 0;
    VkDeviceSize size = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize memoryOffset = 0;
};

std::shared_mutex g_resourceMu;
std::unordered_map<VkDeviceMemory, MemoryInfo>* g_memory = new std::unordered_map<VkDeviceMemory, MemoryInfo>();
std::unordered_map<VkBuffer, BufferInfo>* g_buffers = new std::unordered_map<VkBuffer, BufferInfo>();
std::atomic<uint32_t> g_nextBufferId{1};

// The CPU address of a buffer's bytes, or null when its memory is not mapped.
const uint8_t* MappedBytes(const BufferInfo& b) {
    const auto mem = g_memory->find(b.memory);
    if (mem == g_memory->end() || !mem->second.mappedBase) return nullptr;
    return mem->second.mappedBase + b.memoryOffset;
}

bool FindBuffer(VkBuffer buffer, BufferInfo* out, const uint8_t** mapped) {
    std::shared_lock lock(g_resourceMu);
    const auto it = g_buffers->find(buffer);
    if (it == g_buffers->end()) return false;
    *out = it->second;
    if (mapped) *mapped = MappedBytes(it->second);
    return true;
}

// ---- Images

struct ImageInfo {
    uint64_t id = 0;
    uint32_t format = 0, width = 0, height = 0, mips = 0;
    bool readable = false;  // A texture with TRANSFER_SRC (the layer adds it), for readback.
};
std::shared_mutex g_imageMu;
std::unordered_map<VkImage, ImageInfo>* g_images = new std::unordered_map<VkImage, ImageInfo>();
std::atomic<uint64_t> g_nextImageId{1};

// Image views: the image each shows, for the texture hash of what a set holds.
std::shared_mutex g_viewMu;
std::unordered_map<VkImageView, VkImage>* g_views = new std::unordered_map<VkImageView, VkImage>();

bool ImageOfView(VkImageView view, efmitex::ImageDesc* out) {
    VkImage image;
    {
        std::shared_lock lock(g_viewMu);
        const auto it = g_views->find(view);
        if (it == g_views->end()) return false;
        image = it->second;
    }
    std::shared_lock lock(g_imageMu);
    const auto it = g_images->find(image);
    if (it == g_images->end()) return false;
    const ImageInfo& i = it->second;
    *out = {i.id, image, i.format, i.width, i.height, i.mips, i.readable};
    return true;
}

uint32_t TextureHashOfImage(uint64_t id) { return textures::HashOf(id); }

void OnReadback(const efmitex::ImageDesc& image, const uint8_t* mip0, uint64_t bytes) {
    textures::OnImageUpload(image.id, image.format, image.width, image.height, image.mips, mip0, bytes);
}

void OnReadbackMip(const efmitex::ImageDesc& image, uint32_t mip, const uint8_t* data, uint64_t bytes) {
    textures::OnMipUpload(image.id, image.format, mip, std::max(1u, image.width >> mip), std::max(1u, image.height >> mip), data, bytes);
}

uint32_t TextureHashOfView(VkImageView view) {
    VkImage image;
    {
        std::shared_lock lock(g_viewMu);
        const auto it = g_views->find(view);
        if (it == g_views->end()) return 0;
        image = it->second;
    }
    uint64_t id;
    {
        std::shared_lock lock(g_imageMu);
        const auto it = g_images->find(image);
        if (it == g_images->end()) return 0;
        id = it->second.id;
    }
    return textures::HashOf(id);
}

// ---- Descriptors: what each set was last written with, for efmitex to write again.

struct SetLayoutInfo {
    uint32_t dynamicCount = 0;  // Dynamic buffer descriptors: one dynamic offset each.
    bool updateAfterBind = false;
};
struct TemplateInfo {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    bool updateAfterBind = false;
    std::vector<efmitex::ImageEntry> images;  // Sampled images, with or without a sampler.
    std::vector<VkDescriptorUpdateTemplateEntry> entries;  // All of them: buffers too, for bones (2b).
    size_t dataSize = 0;
};
// Buffer views, for texel buffers in set data.
struct BufferViewInfo {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, range = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
};
std::shared_mutex g_bufferViewMu;
std::unordered_map<VkBufferView, BufferViewInfo>* g_bufferViews = new std::unordered_map<VkBufferView, BufferViewInfo>();
struct SetData {
    VkDescriptorUpdateTemplate tmpl = VK_NULL_HANDLE;
    std::vector<uint8_t> data;
    uint64_t updates = 0;  // Bumped by every update: copies of ours tell they are behind.
};

// The dynamic offsets each set of a pipeline layout takes.
struct DynamicPerSet {
    uint32_t sets = 0;
    uint8_t count[efmitex::kMaxSets]{};
};

std::shared_mutex g_descMu;  // Set layouts, pipeline layouts, templates.
std::unordered_map<VkDescriptorSetLayout, SetLayoutInfo>* g_setLayouts = new std::unordered_map<VkDescriptorSetLayout, SetLayoutInfo>();
std::unordered_map<VkPipelineLayout, DynamicPerSet>* g_pipelineLayouts = new std::unordered_map<VkPipelineLayout, DynamicPerSet>();
std::unordered_map<VkDescriptorUpdateTemplate, TemplateInfo>* g_templates = new std::unordered_map<VkDescriptorUpdateTemplate, TemplateInfo>();
// Bumped when a template or pipeline layout is created or destroyed: each thread keeps the
// last one it looked up, valid while this has not changed. Handles can be used again.
std::atomic<uint64_t> g_descGeneration{1};

// Sets written with templates that hold images, sharded: Unity writes sets from many
// threads at once, ~20k a second.
constexpr size_t kSetShards = 64;
struct SetShard {
    std::mutex mu;
    std::unordered_map<VkDescriptorSet, SetData> sets;
};
SetShard* g_setShards = new SetShard[kSetShards];
SetShard& ShardOf(VkDescriptorSet set) { return g_setShards[(reinterpret_cast<uintptr_t>(set) >> 4) % kSetShards]; }

uint64_t SetUpdates(VkDescriptorSet set) {
    SetShard& shard = ShardOf(set);
    std::lock_guard lock(shard.mu);
    const auto it = shard.sets.find(set);
    return it == shard.sets.end() ? 0 : it->second.updates;
}

bool SetContents(VkDescriptorSet set, efmitex::SetContents* out) {
    VkDescriptorUpdateTemplate tmpl;
    {
        SetShard& shard = ShardOf(set);
        std::lock_guard lock(shard.mu);
        const auto it = shard.sets.find(set);
        if (it == shard.sets.end() || !it->second.tmpl) return false;
        tmpl = it->second.tmpl;
        out->data = it->second.data;
        out->updates = it->second.updates;
    }
    std::shared_lock lock(g_descMu);
    const auto t = g_templates->find(tmpl);
    if (t == g_templates->end()) return false;
    out->tmpl = tmpl;
    out->layout = t->second.layout;
    out->updateAfterBind = t->second.updateAfterBind;
    out->images = t->second.images;
    out->entries = t->second.entries;
    return true;
}

// Bytes one descriptor of a type takes in template data.
size_t TemplateElementSize(VkDescriptorType type) {
    switch (type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            return 8;
        default:
            return sizeof(VkDescriptorImageInfo);  // Image and buffer infos are both 24 bytes.
    }
}

// Mip 0 uploads the texture hash cannot use, by reason, for the stats.
std::atomic<uint64_t> g_skipLayer{0}, g_skipPart{0}, g_skipPitch{0}, g_skipUnmapped{0};
// Of those not mapped at submit: why.
std::atomic<uint64_t> g_unreadGone{0}, g_unreadNoMemory{0}, g_unreadDeviceOnly{0}, g_unreadHostUnmapped{0};

// Which way Unity binds textures: counted, so the log can tell.
std::atomic<uint64_t> g_updateSets{0}, g_updateTemplate{0}, g_pushSets{0}, g_pushTemplate{0}, g_bindSets{0};

// ---- Pipelines: the vertex bindings each reads, and their strides

struct PipelineInfo {
    uint32_t strides[efmidraw::kMaxBindings]{};  // 0: not read.
    uint32_t extents[efmidraw::kMaxBindings]{};  // Where the attributes read end; 0: unknown.
    efmidraw::AttributeLayout layouts[efmidraw::kMaxBindings];  // Each binding's attributes, by offset.
    bool dynamicStride = false;
    uint32_t keepGame = 0;  // Bindings read per instance, or with stride 0 (one element for all).
    VkPipeline handle = VK_NULL_HANDLE;
    // What pass it draws in: culling (vkCmdSetCullMode when dynamic), colour targets, depth writes.
    uint32_t cullMode = 0, frontFace = 0, colorTargets = 0;
    bool dynamicCull = false, depthWrite = false;
};
// Vertex attributes of each pipeline, only for the dump of a replaced draw.
std::unordered_map<VkPipeline, std::vector<VkVertexInputAttributeDescription>>* g_pipelineAttrs =
    new std::unordered_map<VkPipeline, std::vector<VkVertexInputAttributeDescription>>();
std::shared_mutex g_pipelineMu;
std::unordered_map<VkPipeline, PipelineInfo>* g_pipelines = new std::unordered_map<VkPipeline, PipelineInfo>();

// ---- Command buffers

struct PendingCopy {
    const uint8_t* source;  // Null: bytes the CPU cannot see, the range only goes stale.
    uint64_t destination;   // ibregion key.
    uint64_t size;
};
struct PendingImageUpload {
    const uint8_t* source;  // Null: not mapped when recorded, looked up again at submit.
    uint64_t available;     // Bytes the staging buffer holds from source on.
    ImageInfo image;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint32_t mip = 0, width = 0, height = 0;  // The level, and its size.
};

struct CommandState {
    bool graphicsPrimary = false;  // A primary command buffer of a graphics queue family.
    // Since its last begin: the frame it began in, submits (primaries), runs inside primaries
    // (secondaries). A command buffer used again without being recorded again keeps the
    // bones and configs of its first frame, which the layer fills only once.
    uint64_t beginFrame = 0;
    uint32_t submits = 0, executions = 0;
    efmitex::BoundSet sets[efmitex::kMaxSets];  // Bound for graphics.
    uint64_t indexKey = 0;  // ibregion key of the bound index buffer view.
    uint32_t indexBytes = 0;
    const uint8_t* indexMapped = nullptr;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceSize indexOffset = 0;
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;
    efmidraw::VertexBinding vb[efmidraw::kMaxBindings];
    PipelineInfo pipeline;  // The bound graphics pipeline's.
    uint32_t dynamicCullMode = 0;  // vkCmdSetCullMode.
    std::vector<PendingCopy> copies;
    std::vector<PendingImageUpload> imageUploads;
};

std::shared_mutex g_commandMu;
std::unordered_map<VkCommandBuffer, CommandState*>* g_commands = new std::unordered_map<VkCommandBuffer, CommandState*>();

// A command buffer is recorded by one thread at a time, so its state needs no lock,
// and each thread remembers the one it used last.
CommandState* Command(VkCommandBuffer cb) {
    thread_local VkCommandBuffer lastCb = VK_NULL_HANDLE;
    thread_local CommandState* last = nullptr;
    if (cb == lastCb) return last;
    {
        std::shared_lock lock(g_commandMu);
        const auto it = g_commands->find(cb);
        if (it != g_commands->end()) {
            lastCb = cb;
            last = it->second;
            return last;
        }
    }
    std::unique_lock lock(g_commandMu);
    CommandState*& state = (*g_commands)[cb];
    if (!state) state = new CommandState();
    lastCb = cb;
    last = state;
    return state;
}

void RecordCopy(CommandState* s, VkBuffer src, VkBuffer dst, VkDeviceSize srcOffset, VkDeviceSize dstOffset, VkDeviceSize size) {
    BufferInfo from, to;
    const uint8_t* mapped = nullptr;
    if (!FindBuffer(dst, &to, nullptr)) return;
    const bool readable = FindBuffer(src, &from, &mapped) && mapped;
    s->copies.push_back({readable ? mapped + srcOffset : nullptr, ibregion::BufferKey(to.id, dstOffset), size});
}

// At submit the staging bytes are written and not yet reused: read them now.
std::atomic<uint64_t> g_submitted{0}, g_resubmitted{0}, g_reexecuted{0}, g_submitGap[4]{};

void Submitted(VkCommandBuffer cb) {
    CommandState* s = Command(cb);
    g_submitted.fetch_add(1, std::memory_order_relaxed);
    if (++s->submits > 1) {
        g_resubmitted.fetch_add(1, std::memory_order_relaxed);
    } else {
        const uint64_t gap = g_stats.presents.load(std::memory_order_relaxed) - s->beginFrame;
        g_submitGap[std::min<uint64_t>(gap, 3)].fetch_add(1, std::memory_order_relaxed);
    }
    for (const PendingCopy& c : s->copies) {
        Guard("upload", [&] {
            if (c.source) {
                ibregion::OnUpload(c.destination, c.source, c.size);
            } else {
                ibregion::OnOverwritten(c.destination, c.size);
            }
        });
    }
    s->copies.clear();
    // Mip 0 first: a lower level counts once its image matched.
    std::stable_sort(s->imageUploads.begin(), s->imageUploads.end(), [](const PendingImageUpload& a, const PendingImageUpload& b) { return a.mip < b.mip; });
    for (const PendingImageUpload& u : s->imageUploads) {
        Guard("image upload", [&] {
            const uint8_t* source = u.source;
            uint64_t available = u.available;
            if (!source) {
                BufferInfo from;
                const uint8_t* mapped = nullptr;
                if (!FindBuffer(u.buffer, &from, &mapped) || !mapped || u.offset >= from.size) {
                    g_skipUnmapped.fetch_add(1, std::memory_order_relaxed);
                    std::shared_lock lock(g_resourceMu);
                    const auto b = g_buffers->find(u.buffer);
                    const auto m = b == g_buffers->end() ? g_memory->end() : g_memory->find(b->second.memory);
                    if (b == g_buffers->end()) {
                        g_unreadGone.fetch_add(1, std::memory_order_relaxed);
                    } else if (m == g_memory->end()) {
                        g_unreadNoMemory.fetch_add(1, std::memory_order_relaxed);
                    } else if (!m->second.hostVisible) {
                        g_unreadDeviceOnly.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        g_unreadHostUnmapped.fetch_add(1, std::memory_order_relaxed);
                    }
                    return;
                }
                source = mapped + u.offset;
                available = from.size - u.offset;
            }
            if (u.mip == 0) {
                textures::OnImageUpload(u.image.id, u.image.format, u.image.width, u.image.height, u.image.mips, source, available);
            } else {
                textures::OnMipUpload(u.image.id, u.image.format, u.mip, u.width, u.height, source, available);
            }
        });
        if (u.mip == 0) efmitex::OnGameUpload(u.image.id);
    }
    s->imageUploads.clear();
    efmitex::OnSubmitted(cb);
}

// ---- Background thread: keys and stats

void StartBackground() {
    static std::once_flag once;
    std::call_once(once, [] {
        LogInit();
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        LogInfo("efmivulkan milestone 2: meshes, textures, merged skeleton (Vulkan layer), pid %lu, %s", GetCurrentProcessId(), ToUtf8(exe).c_str());
        LogInfo("command line: %s", ToUtf8(GetCommandLineW()).c_str());
        PostJob([] {
            Guard("LoadModIndex", [] { efmi::LoadModIndex(); });
            Guard("LoadMods", [] { efmidraw::LoadMods(); });
        });
        std::thread([] {
            bool f10WasDown = false, f9WasDown = false;
            ULONGLONG lastStats = GetTickCount64();
            for (;;) {
                Sleep(30);
                const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
                if (f10 && !f10WasDown) {
                    efmi::RequestCapture();
                    MessageBeep(MB_ICONASTERISK);
                }
                f10WasDown = f10;
                const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
                if (f9 && !f9WasDown) {
                    const bool on = efmidraw::ToggleEnabled();
                    LogInfo("mods %s", on ? "on" : "off");
                    MessageBeep(on ? MB_OK : MB_ICONHAND);
                }
                f9WasDown = f9;
                Guard("PollKeys", [] { efmidraw::PollKeys(); });
                if (GetTickCount64() - lastStats >= 15000) {
                    lastStats = GetTickCount64();
                    Guard("LogStats", [] {
                        efmi::LogStats();
                        textures::LogStats();
                        LogInfo("textures: mip 0 uploads not hashed: %llu to array layers, %llu partial, %llu with row padding, %llu from memory the "
                                "CPU could not read at submit",
                                (unsigned long long)g_skipLayer.load(), (unsigned long long)g_skipPart.load(), (unsigned long long)g_skipPitch.load(),
                                (unsigned long long)g_skipUnmapped.load());
                        LogInfo("textures: of those, staging buffer gone %llu, memory unknown %llu, GPU-only memory %llu, CPU-visible but "
                                "not mapped %llu",
                                (unsigned long long)g_unreadGone.load(), (unsigned long long)g_unreadNoMemory.load(),
                                (unsigned long long)g_unreadDeviceOnly.load(), (unsigned long long)g_unreadHostUnmapped.load());
                        efmibones::LogStats();
                        efmidraw::LogStats();
                        LogInfo("command buffers: %llu submitted, %llu of them again without being recorded again; secondaries run again "
                                "without being recorded again %llu; frames from begin to submit: 0 x%llu, 1 x%llu, 2 x%llu, 3+ x%llu",
                                (unsigned long long)g_submitted.load(), (unsigned long long)g_resubmitted.load(),
                                (unsigned long long)g_reexecuted.load(), (unsigned long long)g_submitGap[0].load(),
                                (unsigned long long)g_submitGap[1].load(), (unsigned long long)g_submitGap[2].load(),
                                (unsigned long long)g_submitGap[3].load());
                        efmitex::LogStats();
                        LogInfo("descriptors: vkUpdateDescriptorSets %llu, with templates %llu, pushed %llu, pushed with templates %llu; "
                                "vkCmdBindDescriptorSets %llu",
                                (unsigned long long)g_updateSets.load(), (unsigned long long)g_updateTemplate.load(),
                                (unsigned long long)g_pushSets.load(), (unsigned long long)g_pushTemplate.load(), (unsigned long long)g_bindSets.load());
                    });
                }
            }
        }).detach();
    });
}

// ---- Intercepted device calls

VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice device, const VkBufferCreateInfo* info, const VkAllocationCallbacks* alloc, VkBuffer* buffer) {
    DeviceData* d = Device(device);
    const VkResult r = d->CreateBuffer(device, info, alloc, buffer);
    if (r == VK_SUCCESS) {
        std::unique_lock lock(g_resourceMu);
        (*g_buffers)[*buffer] = BufferInfo{g_nextBufferId.fetch_add(1, std::memory_order_relaxed), info->size};
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* alloc) {
    if (buffer) {
        BufferInfo b;
        bool found;
        {
            std::unique_lock lock(g_resourceMu);
            const auto it = g_buffers->find(buffer);
            found = it != g_buffers->end();
            if (found) {
                b = it->second;
                g_buffers->erase(it);
            }
        }
        if (found) Guard("DestroyBuffer", [&] { ibregion::OnOverwritten(ibregion::BufferKey(b.id, 0), b.size); });
    }
    Device(device)->DestroyBuffer(device, buffer, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* alloc, VkDeviceMemory* memory) {
    DeviceData* d = Device(device);
    const VkResult r = d->AllocateMemory(device, info, alloc, memory);
    if (r == VK_SUCCESS && info->memoryTypeIndex < d->memory.memoryTypeCount) {
        const bool host = (d->memory.memoryTypes[info->memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        std::unique_lock lock(g_resourceMu);
        (*g_memory)[*memory] = MemoryInfo{info->allocationSize, host, nullptr};
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* alloc) {
    {
        std::unique_lock lock(g_resourceMu);
        g_memory->erase(memory);
    }
    Device(device)->FreeMemory(device, memory, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL MapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void** data) {
    const VkResult r = Device(device)->MapMemory(device, memory, offset, size, flags, data);
    if (r == VK_SUCCESS && data && *data) {
        std::unique_lock lock(g_resourceMu);
        const auto it = g_memory->find(memory);
        if (it != g_memory->end()) it->second.mappedBase = static_cast<uint8_t*>(*data) - offset;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL UnmapMemory(VkDevice device, VkDeviceMemory memory) {
    {
        std::unique_lock lock(g_resourceMu);
        const auto it = g_memory->find(memory);
        if (it != g_memory->end()) it->second.mappedBase = nullptr;
    }
    Device(device)->UnmapMemory(device, memory);
}

// vkMapMemory2 and vkUnmapMemory2: as vkMapMemory and vkUnmapMemory.
void Mapped2(const VkMemoryMapInfoKHR* info, void** data) {
    std::unique_lock lock(g_resourceMu);
    const auto it = g_memory->find(info->memory);
    if (it != g_memory->end()) it->second.mappedBase = static_cast<uint8_t*>(*data) - info->offset;
}
void Unmapped2(const VkMemoryUnmapInfoKHR* info) {
    std::unique_lock lock(g_resourceMu);
    const auto it = g_memory->find(info->memory);
    if (it != g_memory->end()) it->second.mappedBase = nullptr;
}
VKAPI_ATTR VkResult VKAPI_CALL MapMemory2(VkDevice device, const VkMemoryMapInfoKHR* info, void** data) {
    const VkResult r = Device(device)->MapMemory2(device, info, data);
    if (r == VK_SUCCESS && data && *data) Mapped2(info, data);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL MapMemory2KHR(VkDevice device, const VkMemoryMapInfoKHR* info, void** data) {
    const VkResult r = Device(device)->MapMemory2KHR(device, info, data);
    if (r == VK_SUCCESS && data && *data) Mapped2(info, data);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL UnmapMemory2(VkDevice device, const VkMemoryUnmapInfoKHR* info) {
    Unmapped2(info);
    return Device(device)->UnmapMemory2(device, info);
}
VKAPI_ATTR VkResult VKAPI_CALL UnmapMemory2KHR(VkDevice device, const VkMemoryUnmapInfoKHR* info) {
    Unmapped2(info);
    return Device(device)->UnmapMemory2KHR(device, info);
}

void Bound(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    std::unique_lock lock(g_resourceMu);
    const auto it = g_buffers->find(buffer);
    if (it == g_buffers->end()) return;
    it->second.memory = memory;
    it->second.memoryOffset = offset;
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    const VkResult r = Device(device)->BindBufferMemory(device, buffer, memory, offset);
    if (r == VK_SUCCESS) Bound(buffer, memory, offset);
    return r;
}

VkResult BindBufferMemory2Common(PFN_vkBindBufferMemory2 next, VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos) {
    const VkResult r = next(device, count, infos);
    if (r == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; ++i) Bound(infos[i].buffer, infos[i].memory, infos[i].memoryOffset);
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos) {
    return BindBufferMemory2Common(Device(device)->BindBufferMemory2, device, count, infos);
}
VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2KHR(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos) {
    return BindBufferMemory2Common(Device(device)->BindBufferMemory2KHR, device, count, infos);
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* info) {
    CommandState* s = Command(cb);
    s->indexKey = 0;
    s->indexBytes = 0;
    s->indexMapped = nullptr;
    s->indexBuffer = VK_NULL_HANDLE;
    for (auto& v : s->vb) v = {};
    s->pipeline = {};
    s->copies.clear();
    s->imageUploads.clear();
    for (auto& set : s->sets) set = {};
    s->beginFrame = g_stats.presents.load(std::memory_order_relaxed);
    s->submits = s->executions = 0;
    const VkResult r = Device(cb)->BeginCommandBuffer(cb, info);
    if (r == VK_SUCCESS && s->graphicsPrimary) Guard("efmitex::OnBegin", [&] { efmitex::OnBegin(cb); });
    Guard("efmibones::OnBegin", [&] { efmibones::OnBegin(cb); });
    return r;
}

VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t count, const VkBufferCopy* regions) {
    Guard("CmdCopyBuffer", [&] {
        CommandState* s = Command(cb);
        for (uint32_t i = 0; i < count; ++i) RecordCopy(s, src, dst, regions[i].srcOffset, regions[i].dstOffset, regions[i].size);
    });
    Device(cb)->CmdCopyBuffer(cb, src, dst, count, regions);
}

void CopyBuffer2Common(VkCommandBuffer cb, const VkCopyBufferInfo2* info) {
    Guard("CmdCopyBuffer2", [&] {
        CommandState* s = Command(cb);
        for (uint32_t i = 0; i < info->regionCount; ++i) {
            const VkBufferCopy2& r = info->pRegions[i];
            RecordCopy(s, info->srcBuffer, info->dstBuffer, r.srcOffset, r.dstOffset, r.size);
        }
    });
}
VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer2(VkCommandBuffer cb, const VkCopyBufferInfo2* info) {
    CopyBuffer2Common(cb, info);
    Device(cb)->CmdCopyBuffer2(cb, info);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer2KHR(VkCommandBuffer cb, const VkCopyBufferInfo2* info) {
    CopyBuffer2Common(cb, info);
    Device(cb)->CmdCopyBuffer2KHR(cb, info);
}

VKAPI_ATTR void VKAPI_CALL CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
    Guard("CmdBindIndexBuffer", [&] {
        CommandState* s = Command(cb);
        s->indexBuffer = buffer;
        s->indexOffset = offset;
        s->indexType = type;
        BufferInfo b;
        const uint8_t* mapped = nullptr;
        if (buffer && FindBuffer(buffer, &b, &mapped)) {
            s->indexKey = ibregion::BufferKey(b.id, offset);
            s->indexBytes = type == VK_INDEX_TYPE_UINT32 ? 4 : type == VK_INDEX_TYPE_UINT16 ? 2 : 0;
            s->indexMapped = mapped ? mapped + offset : nullptr;
        } else {
            s->indexKey = 0;
            s->indexBytes = 0;
            s->indexMapped = nullptr;
        }
    });
    Device(cb)->CmdBindIndexBuffer(cb, buffer, offset, type);
}

std::string Hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; ++i) {
        snprintf(b, sizeof(b), "%02x%s", p[i], (i % 4 == 3 && i + 1 < n) ? " " : "");
        s += b;
    }
    return s;
}

const char* DescriptorTypeName(VkDescriptorType t) {
    switch (t) {
        case VK_DESCRIPTOR_TYPE_SAMPLER: return "sampler";
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return "image+sampler";
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return "image";
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return "storage image";
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return "uniform texel buffer";
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return "storage texel buffer";
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return "uniform buffer";
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return "storage buffer";
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: return "uniform buffer dynamic";
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return "storage buffer dynamic";
        default: return "other";
    }
}

// A buffer's range as the dump shows it: its id and size, whether the CPU can read it,
// and its first bytes, plus row 5 of the instance config when it could be one.
void DumpBufferRange(std::string& text, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range, uint32_t firstInstance) {
    char line[256];
    BufferInfo info;
    const uint8_t* mapped = nullptr;
    if (!buffer || !FindBuffer(buffer, &info, &mapped)) {
        text += " (unknown buffer)\n";
        return;
    }
    if (range == VK_WHOLE_SIZE) range = info.size - offset;
    snprintf(line, sizeof(line), " buffer %u (%llu bytes) +%llu, %llu bytes, %s\n", info.id, (unsigned long long)info.size,
             (unsigned long long)offset, (unsigned long long)range, mapped ? "mapped" : "not mapped");
    text += line;
    if (!mapped || offset >= info.size) return;
    text += "        +0: " + Hex(mapped + offset, static_cast<size_t>(std::min<VkDeviceSize>(32, info.size - offset))) + "\n";
    // Instance config: 16 rows of 16 bytes per instance; row 5 has the bone offsets.
    for (const uint64_t row : {uint64_t(5), uint64_t(firstInstance) * 16 + 5}) {
        const uint64_t at = offset + row * 16;
        if (row * 16 + 16 > range || at + 16 > info.size) continue;
        snprintf(line, sizeof(line), "        row %llu: ", (unsigned long long)row);
        text += line + Hex(mapped + at, 16) + "\n";
        if (row == 5 && firstInstance == 0) break;
    }
}

void DumpBoundSets(std::string& text, const CommandState* s, uint32_t firstInstance) {
    char line[256];
    for (uint32_t i = 0; i < efmitex::kMaxSets; ++i) {
        const efmitex::BoundSet& b = s->sets[i];
        if (!b.set) continue;
        VkDescriptorUpdateTemplate tmpl = VK_NULL_HANDLE;
        std::vector<uint8_t> data;
        {
            SetShard& shard = ShardOf(b.set);
            std::lock_guard lock(shard.mu);
            const auto it = shard.sets.find(b.set);
            if (it != shard.sets.end()) {
                tmpl = it->second.tmpl;
                data = it->second.data;
            }
        }
        std::vector<VkDescriptorUpdateTemplateEntry> entries;
        {
            std::shared_lock lock(g_descMu);
            const auto t = g_templates->find(tmpl);
            if (t != g_templates->end()) entries = t->second.entries;
        }
        snprintf(line, sizeof(line), "  set %u: %u dynamic offsets%s\n", i, b.dynamicCount, tmpl ? "" : ", contents unknown");
        text += line;
        // Dynamic offsets go by binding, then array element.
        std::vector<std::pair<uint64_t, uint32_t>> dynamicOrder;  // (binding << 32 | element), index in order.
        for (const auto& e : entries) {
            if (e.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && e.descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) continue;
            for (uint32_t k = 0; k < e.descriptorCount; ++k) dynamicOrder.push_back({(uint64_t(e.dstBinding) << 32) | (e.dstArrayElement + k), 0});
        }
        std::sort(dynamicOrder.begin(), dynamicOrder.end());
        for (const auto& e : entries) {
            for (uint32_t k = 0; k < e.descriptorCount && k < 4; ++k) {
                const size_t at = e.offset + size_t(k) * e.stride;
                snprintf(line, sizeof(line), "    binding %u[%u] %s:", e.dstBinding, e.dstArrayElement + k, DescriptorTypeName(e.descriptorType));
                text += line;
                switch (e.descriptorType) {
                    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
                        if (at + sizeof(VkDescriptorBufferInfo) > data.size()) {
                            text += " (no data)\n";
                            break;
                        }
                        VkDescriptorBufferInfo bi;
                        memcpy(&bi, data.data() + at, sizeof(bi));
                        VkDeviceSize offset = bi.offset;
                        if (e.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || e.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                            const uint64_t key = (uint64_t(e.dstBinding) << 32) | (e.dstArrayElement + k);
                            const auto d = std::lower_bound(dynamicOrder.begin(), dynamicOrder.end(), std::make_pair(key, 0u));
                            const size_t index = d - dynamicOrder.begin();
                            if (index < b.dynamicCount) {
                                offset += b.dynamic[index];
                                snprintf(line, sizeof(line), " (dynamic +%u)", b.dynamic[index]);
                                text += line;
                            }
                        }
                        DumpBufferRange(text, bi.buffer, offset, bi.range, firstInstance);
                        break;
                    }
                    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
                        if (at + sizeof(VkBufferView) > data.size()) {
                            text += " (no data)\n";
                            break;
                        }
                        VkBufferView view;
                        memcpy(&view, data.data() + at, sizeof(view));
                        BufferViewInfo v;
                        {
                            std::shared_lock lock(g_bufferViewMu);
                            const auto it = g_bufferViews->find(view);
                            if (it != g_bufferViews->end()) v = it->second;
                        }
                        snprintf(line, sizeof(line), " format %u", v.format);
                        text += line;
                        DumpBufferRange(text, v.buffer, v.offset, v.range, firstInstance);
                        break;
                    }
                    default:
                        text += "\n";
                }
            }
        }
    }
}

// Once per hash: what the game's draw binds, to compare with the mod's buffers by hand.
void DumpReplacedDraw(const CommandState* s, uint32_t hash, uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    thread_local std::set<uint32_t> seen;  // No lock for hashes this thread has seen.
    if (!seen.insert(hash).second) return;
    static std::mutex mu;
    static std::set<uint32_t> done;
    {
        std::lock_guard lock(mu);
        if (done.size() >= 96 || !done.insert(hash).second) return;
    }
    std::string text;
    char line[256];
    uint32_t first[3]{};
    if (s->indexMapped) {
        for (uint32_t i = 0; i < 3 && i < indexCount; ++i) {
            first[i] = s->indexBytes == 4 ? reinterpret_cast<const uint32_t*>(s->indexMapped)[firstIndex + i]
                                          : reinterpret_cast<const uint16_t*>(s->indexMapped)[firstIndex + i];
        }
    }
    snprintf(line, sizeof(line), "dump %08x: count=%u first=%u vertexOffset=%d, first indices %u %u %u%s\n", hash, indexCount, firstIndex,
             vertexOffset, first[0], first[1], first[2], s->indexMapped ? "" : " (index data not mapped)");
    text += line;
    std::vector<VkVertexInputAttributeDescription> attrs;
    {
        std::shared_lock lock(g_pipelineMu);
        const auto it = g_pipelineAttrs->find(s->pipeline.handle);
        if (it != g_pipelineAttrs->end()) attrs = it->second;
    }
    for (uint32_t b = 0; b < efmidraw::kMaxBindings; ++b) {
        if (!s->pipeline.strides[b]) continue;
        const uint32_t stride = s->pipeline.strides[b];
        snprintf(line, sizeof(line), "  binding %u stride %u%s:", b, stride, (s->pipeline.keepGame & (1u << b)) ? " (kept)" : "");
        text += line;
        for (const auto& a : attrs) {
            if (a.binding != b) continue;
            snprintf(line, sizeof(line), " loc%u fmt%u +%u", a.location, a.format, a.offset);
            text += line;
        }
        text += "\n";
        BufferInfo info;
        const uint8_t* mapped = nullptr;
        if (!s->vb[b].buffer || !FindBuffer(s->vb[b].buffer, &info, &mapped) || !mapped) {
            text += "    (not mapped)\n";
            continue;
        }
        for (uint32_t k = 0; k < 3; ++k) {
            const uint64_t at = s->vb[b].offset + uint64_t(int64_t(first[k]) + vertexOffset) * stride;
            if (at + stride > info.size) break;
            snprintf(line, sizeof(line), "    v%u: ", first[k]);
            text += line + Hex(mapped + at, std::min<uint32_t>(stride, 48)) + "\n";
        }
    }
    DumpBoundSets(text, s, firstInstance);
    for (size_t at = 0, end; at < text.size(); at = end + 1) {  // A line each: the log's lines are short.
        end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        LogInfo("%s", text.substr(at, end - at).c_str());
    }
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer cb, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset,
                                          uint32_t firstInstance) {
    bool replaced = false;
    Guard("CmdDrawIndexed", [&] {
        const CommandState* s = Command(cb);
        if (!s->indexKey) return;
        const uint32_t hash =
            efmi::OnIndexedDraw({s->indexKey, s->indexBytes, indexCount, instanceCount, firstIndex, firstInstance, vertexOffset, s->indexMapped});
        if (!hash) return;
        const efmidraw::BindState state{s->indexBuffer, s->indexOffset, s->indexType, s->vb, s->pipeline.strides, s->pipeline.dynamicStride,
                                        s->pipeline.keepGame, s->sets,
                                        s->pipeline.dynamicCull ? s->dynamicCullMode : s->pipeline.cullMode, s->pipeline.frontFace,
                                        s->pipeline.colorTargets, s->pipeline.depthWrite, reinterpret_cast<uint64_t>(s->pipeline.handle),
                                        s->pipeline.extents, s->pipeline.layouts};
        DumpReplacedDraw(s, hash, indexCount, firstIndex, vertexOffset, firstInstance);  // Before: the mod's draw changes nothing it reads.
        replaced = efmidraw::TryReplace(cb, {hash, indexCount, instanceCount, firstInstance, firstIndex, vertexOffset}, state);
    });
    if (!replaced) Device(cb)->CmdDrawIndexed(cb, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo* info, const VkAllocationCallbacks* alloc, VkImage* image) {
    // Textures (sampled, single layer, not something rendered to) can be copied from, so the
    // layer can read back the ones whose pixels it never saw go up (efmitex readback).
    constexpr VkImageUsageFlags kRendered = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                           VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    const bool texture = info->imageType == VK_IMAGE_TYPE_2D && (info->usage & VK_IMAGE_USAGE_SAMPLED_BIT) && !(info->usage & kRendered) &&
                         info->arrayLayers == 1 && info->samples == VK_SAMPLE_COUNT_1_BIT && info->tiling == VK_IMAGE_TILING_OPTIMAL &&
                         texhash::MipBytes(info->format, 4, 4) != 0;
    VkImageCreateInfo copy = *info;
    if (texture) copy.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // Read back, written in place.
    VkResult r = Device(device)->CreateImage(device, &copy, alloc, image);
    if (r != VK_SUCCESS && texture) r = Device(device)->CreateImage(device, info, alloc, image);  // As the game asked, then.
    if (r == VK_SUCCESS && info->imageType == VK_IMAGE_TYPE_2D) {
        std::unique_lock lock(g_imageMu);
        ImageInfo i{g_nextImageId.fetch_add(1, std::memory_order_relaxed), static_cast<uint32_t>(info->format), info->extent.width, info->extent.height,
                    info->mipLevels};
        i.readable = texture;
        (*g_images)[*image] = i;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* alloc) {
    uint64_t id = 0;
    {
        std::unique_lock lock(g_imageMu);
        const auto it = g_images->find(image);
        if (it != g_images->end()) {
            id = it->second.id;
            g_images->erase(it);
        }
    }
    if (id) {
        textures::OnImageDestroyed(id);
        efmitex::OnImageDestroyed(id);
    }
    Device(device)->DestroyImage(device, image, alloc);
}

// An upload of a whole mip 0, tightly packed, from memory the CPU can read: hashed at submit.
void RecordImageUpload(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkDeviceSize offset, uint32_t rowLength, uint32_t imageHeight,
                       const VkImageSubresourceLayers& sub, const VkOffset3D& at, const VkExtent3D& extent) {
    if (sub.baseArrayLayer != 0) {
        g_skipLayer.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ImageInfo image;
    {
        std::shared_lock lock(g_imageMu);
        const auto it = g_images->find(dst);
        if (it == g_images->end()) return;
        image = it->second;
    }
    // A whole level, tightly packed: mip 0 for the hash, lower ones for streamed copies.
    const uint32_t width = std::max(1u, image.width >> sub.mipLevel), height = std::max(1u, image.height >> sub.mipLevel);
    if (at.x || at.y || at.z || extent.width != width || extent.height != height) {
        if (sub.mipLevel == 0) g_skipPart.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Block-compressed levels below 4 texels are given padded to the block.
    const uint32_t pitch = (rowLength && rowLength < 4) ? width : rowLength;
    if ((pitch && pitch != width && pitch != ((width + 3) & ~3u)) || (imageHeight && imageHeight != height && imageHeight != ((height + 3) & ~3u))) {
        if (sub.mipLevel == 0) g_skipPitch.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    BufferInfo from;
    const uint8_t* mapped = nullptr;
    if (!FindBuffer(src, &from, &mapped) || offset >= from.size) return;
    PendingImageUpload u{mapped ? mapped + offset : nullptr, mapped ? from.size - offset : 0, image, src, offset};  // Unmapped: maybe by submit.
    u.mip = sub.mipLevel;
    u.width = width;
    u.height = height;
    Command(cb)->imageUploads.push_back(u);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout layout, uint32_t count,
                                                const VkBufferImageCopy* regions) {
    Guard("CmdCopyBufferToImage", [&] {
        for (uint32_t i = 0; i < count; ++i) {
            const VkBufferImageCopy& r = regions[i];
            RecordImageUpload(cb, src, dst, r.bufferOffset, r.bufferRowLength, r.bufferImageHeight, r.imageSubresource, r.imageOffset, r.imageExtent);
        }
    });
    Device(cb)->CmdCopyBufferToImage(cb, src, dst, layout, count, regions);
}

void CopyBufferToImage2Common(VkCommandBuffer cb, const VkCopyBufferToImageInfo2* info) {
    Guard("CmdCopyBufferToImage2", [&] {
        for (uint32_t i = 0; i < info->regionCount; ++i) {
            const VkBufferImageCopy2& r = info->pRegions[i];
            RecordImageUpload(cb, info->srcBuffer, info->dstImage, r.bufferOffset, r.bufferRowLength, r.bufferImageHeight, r.imageSubresource,
                              r.imageOffset, r.imageExtent);
        }
    });
}
VKAPI_ATTR void VKAPI_CALL CmdCopyBufferToImage2(VkCommandBuffer cb, const VkCopyBufferToImageInfo2* info) {
    CopyBufferToImage2Common(cb, info);
    Device(cb)->CmdCopyBufferToImage2(cb, info);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyBufferToImage2KHR(VkCommandBuffer cb, const VkCopyBufferToImageInfo2* info) {
    CopyBufferToImage2Common(cb, info);
    Device(cb)->CmdCopyBufferToImage2KHR(cb, info);
}

VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(VkDevice device, uint32_t writeCount, const VkWriteDescriptorSet* writes, uint32_t copyCount,
                                                const VkCopyDescriptorSet* copies) {
    g_updateSets.fetch_add(1, std::memory_order_relaxed);
    Device(device)->UpdateDescriptorSets(device, writeCount, writes, copyCount, copies);
}
// Bytes of data to keep for sets written with a template: 0 for one the layer does not know.
size_t TemplateRecordSize(VkDescriptorUpdateTemplate tmpl) {
    thread_local VkDescriptorUpdateTemplate lastTmpl = VK_NULL_HANDLE;
    thread_local uint64_t lastGeneration = 0;
    thread_local size_t lastSize = 0;
    const uint64_t generation = g_descGeneration.load(std::memory_order_acquire);
    if (tmpl == lastTmpl && generation == lastGeneration) return lastSize;
    size_t size = 0;
    {
        std::shared_lock lock(g_descMu);
        const auto t = g_templates->find(tmpl);
        if (t != g_templates->end()) size = t->second.dataSize;
    }
    lastTmpl = tmpl;
    lastGeneration = generation;
    lastSize = size;
    return size;
}

void RecordTemplateUpdate(VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl, const void* data) {
    g_updateTemplate.fetch_add(1, std::memory_order_relaxed);
    const size_t size = data ? TemplateRecordSize(tmpl) : 0;
    SetShard& shard = ShardOf(set);
    std::lock_guard lock(shard.mu);
    if (!size) {
        // A set handle used again, now without textures. Marked, not erased: no frees.
        const auto it = shard.sets.find(set);
        if (it != shard.sets.end()) {
            it->second.tmpl = VK_NULL_HANDLE;
            ++it->second.updates;
        }
        return;
    }
    SetData& s = shard.sets[set];
    ++s.updates;
    s.tmpl = tmpl;
    s.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);  // Keeps its capacity.
}

// The dynamic offsets each set of a pipeline layout takes, for the thread's last layout.
const DynamicPerSet& DynamicOffsetsOf(VkPipelineLayout layout) {
    thread_local VkPipelineLayout lastLayout = VK_NULL_HANDLE;
    thread_local uint64_t lastGeneration = 0;
    thread_local DynamicPerSet last;
    const uint64_t generation = g_descGeneration.load(std::memory_order_acquire);
    if (layout == lastLayout && generation == lastGeneration) return last;
    {
        std::shared_lock lock(g_descMu);
        const auto it = g_pipelineLayouts->find(layout);
        last = it == g_pipelineLayouts->end() ? DynamicPerSet{} : it->second;
    }
    lastLayout = layout;
    lastGeneration = generation;
    return last;
}
VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl, const void* data) {
    Guard("UpdateDescriptorSetWithTemplate", [&] { RecordTemplateUpdate(set, tmpl, data); });
    Device(device)->UpdateDescriptorSetWithTemplate(device, set, tmpl, data);
}
VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSetWithTemplateKHR(VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl, const void* data) {
    Guard("UpdateDescriptorSetWithTemplate", [&] { RecordTemplateUpdate(set, tmpl, data); });
    Device(device)->UpdateDescriptorSetWithTemplateKHR(device, set, tmpl, data);
}
VKAPI_ATTR void VKAPI_CALL CmdPushDescriptorSetKHR(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t set,
                                                   uint32_t count, const VkWriteDescriptorSet* writes) {
    g_pushSets.fetch_add(1, std::memory_order_relaxed);
    Device(cb)->CmdPushDescriptorSetKHR(cb, point, layout, set, count, writes);
}
VKAPI_ATTR void VKAPI_CALL CmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer cb, VkDescriptorUpdateTemplate tmpl, VkPipelineLayout layout,
                                                               uint32_t set, const void* data) {
    g_pushTemplate.fetch_add(1, std::memory_order_relaxed);
    Device(cb)->CmdPushDescriptorSetWithTemplateKHR(cb, tmpl, layout, set, data);
}
VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first,
                                                 uint32_t count, const VkDescriptorSet* sets, uint32_t dynamicCount, const uint32_t* dynamicOffsets) {
    g_bindSets.fetch_add(1, std::memory_order_relaxed);
    if (point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        Guard("CmdBindDescriptorSets", [&] {
            CommandState* s = Command(cb);
            static const DynamicPerSet kNone;
            const DynamicPerSet& perSet = dynamicCount ? DynamicOffsetsOf(layout) : kNone;
            uint32_t next = 0;
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t index = first + i;
                const uint32_t n = index < perSet.sets && index < efmitex::kMaxSets ? perSet.count[index] : 0;
                if (index < efmitex::kMaxSets) {
                    efmitex::BoundSet& b = s->sets[index];
                    b.set = sets[i];
                    b.layout = layout;
                    b.dynamicCount = n <= efmitex::kMaxDynamic && next + n <= dynamicCount ? n : 0;
                    for (uint32_t k = 0; k < b.dynamicCount; ++k) b.dynamic[k] = dynamicOffsets[next + k];
                }
                next += n;
            }
        });
    }
    Device(cb)->CmdBindDescriptorSets(cb, point, layout, first, count, sets, dynamicCount, dynamicOffsets);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice device, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* alloc, VkImageView* view) {
    const VkResult r = Device(device)->CreateImageView(device, info, alloc, view);
    if (r == VK_SUCCESS) {
        std::unique_lock lock(g_viewMu);
        (*g_views)[*view] = info->image;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks* alloc) {
    {
        std::unique_lock lock(g_viewMu);
        g_views->erase(view);
    }
    Device(device)->DestroyImageView(device, view, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateBufferView(VkDevice device, const VkBufferViewCreateInfo* info, const VkAllocationCallbacks* alloc,
                                                VkBufferView* view) {
    const VkResult r = Device(device)->CreateBufferView(device, info, alloc, view);
    if (r == VK_SUCCESS) {
        std::unique_lock lock(g_bufferViewMu);
        (*g_bufferViews)[*view] = {info->buffer, info->offset, info->range, info->format};
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyBufferView(VkDevice device, VkBufferView view, const VkAllocationCallbacks* alloc) {
    {
        std::unique_lock lock(g_bufferViewMu);
        g_bufferViews->erase(view);
    }
    Device(device)->DestroyBufferView(device, view, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* info, const VkAllocationCallbacks* alloc,
                                                         VkDescriptorSetLayout* layout) {
    const VkResult r = Device(device)->CreateDescriptorSetLayout(device, info, alloc, layout);
    if (r == VK_SUCCESS) {
        SetLayoutInfo l;
        l.updateAfterBind = (info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) != 0;
        for (uint32_t i = 0; i < info->bindingCount; ++i) {
            const VkDescriptorType type = info->pBindings[i].descriptorType;
            if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                l.dynamicCount += info->pBindings[i].descriptorCount;
            }
        }
        std::unique_lock lock(g_descMu);
        (*g_setLayouts)[*layout] = l;
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* info, const VkAllocationCallbacks* alloc,
                                                    VkPipelineLayout* layout) {
    const VkResult r = Device(device)->CreatePipelineLayout(device, info, alloc, layout);
    if (r == VK_SUCCESS) {
        std::unique_lock lock(g_descMu);
        DynamicPerSet perSet;
        perSet.sets = std::min(info->setLayoutCount, efmitex::kMaxSets);
        for (uint32_t i = 0; i < perSet.sets; ++i) {
            const auto it = g_setLayouts->find(info->pSetLayouts[i]);
            perSet.count[i] = static_cast<uint8_t>(std::min<uint32_t>(it == g_setLayouts->end() ? 0 : it->second.dynamicCount, 255));
        }
        (*g_pipelineLayouts)[*layout] = perSet;
        g_descGeneration.fetch_add(1, std::memory_order_release);
    }
    return r;
}

void RecordTemplate(const VkDescriptorUpdateTemplateCreateInfo* info, VkDescriptorUpdateTemplate tmpl) {
    if (info->templateType != VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET) return;
    TemplateInfo t;
    t.layout = info->descriptorSetLayout;
    for (uint32_t i = 0; i < info->descriptorUpdateEntryCount; ++i) {
        const VkDescriptorUpdateTemplateEntry& e = info->pDescriptorUpdateEntries[i];
        if (!e.descriptorCount) continue;
        if (e.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
            t.dataSize = std::max(t.dataSize, e.offset + e.descriptorCount);
            continue;
        }
        t.dataSize = std::max(t.dataSize, e.offset + (e.descriptorCount - 1) * e.stride + TemplateElementSize(e.descriptorType));
        t.entries.push_back(e);
        if (e.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || e.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            t.images.push_back({static_cast<uint32_t>(e.offset), static_cast<uint32_t>(e.stride), e.descriptorCount});
        }
    }
    std::unique_lock lock(g_descMu);
    const auto l = g_setLayouts->find(t.layout);
    t.updateAfterBind = l != g_setLayouts->end() && l->second.updateAfterBind;
    (*g_templates)[tmpl] = std::move(t);
    g_descGeneration.fetch_add(1, std::memory_order_release);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorUpdateTemplate(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* info,
                                                              const VkAllocationCallbacks* alloc, VkDescriptorUpdateTemplate* tmpl) {
    const VkResult r = Device(device)->CreateDescriptorUpdateTemplate(device, info, alloc, tmpl);
    if (r == VK_SUCCESS) Guard("CreateDescriptorUpdateTemplate", [&] { RecordTemplate(info, *tmpl); });
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorUpdateTemplateKHR(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* info,
                                                                 const VkAllocationCallbacks* alloc, VkDescriptorUpdateTemplate* tmpl) {
    const VkResult r = Device(device)->CreateDescriptorUpdateTemplateKHR(device, info, alloc, tmpl);
    if (r == VK_SUCCESS) Guard("CreateDescriptorUpdateTemplate", [&] { RecordTemplate(info, *tmpl); });
    return r;
}
void ForgetTemplate(VkDescriptorUpdateTemplate tmpl) {
    std::unique_lock lock(g_descMu);
    g_templates->erase(tmpl);
    g_descGeneration.fetch_add(1, std::memory_order_release);
}
VKAPI_ATTR void VKAPI_CALL DestroyDescriptorUpdateTemplate(VkDevice device, VkDescriptorUpdateTemplate tmpl, const VkAllocationCallbacks* alloc) {
    ForgetTemplate(tmpl);
    Device(device)->DestroyDescriptorUpdateTemplate(device, tmpl, alloc);
}
VKAPI_ATTR void VKAPI_CALL DestroyDescriptorUpdateTemplateKHR(VkDevice device, VkDescriptorUpdateTemplate tmpl, const VkAllocationCallbacks* alloc) {
    ForgetTemplate(tmpl);
    Device(device)->DestroyDescriptorUpdateTemplateKHR(device, tmpl, alloc);
}

// Command pools: which queue family's command buffers each makes.
std::shared_mutex g_poolMu;
std::unordered_map<VkCommandPool, bool>* g_graphicsPools = new std::unordered_map<VkCommandPool, bool>();

VKAPI_ATTR VkResult VKAPI_CALL CreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo* info, const VkAllocationCallbacks* alloc,
                                                 VkCommandPool* pool) {
    DeviceData* d = Device(device);
    const VkResult r = d->CreateCommandPool(device, info, alloc, pool);
    if (r == VK_SUCCESS) {
        const bool graphics = info->queueFamilyIndex < d->families.size() && (d->families[info->queueFamilyIndex] & VK_QUEUE_GRAPHICS_BIT);
        std::unique_lock lock(g_poolMu);
        (*g_graphicsPools)[*pool] = graphics;
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* cbs) {
    const VkResult r = Device(device)->AllocateCommandBuffers(device, info, cbs);
    if (r == VK_SUCCESS) {
        bool graphics;
        {
            std::shared_lock lock(g_poolMu);
            const auto it = g_graphicsPools->find(info->commandPool);
            graphics = it != g_graphicsPools->end() && it->second;
        }
        for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
            Command(cbs[i])->graphicsPrimary = graphics && info->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        }
    }
    return r;
}

// Bytes of a vertex attribute format; 0 when not one of these.
uint32_t VertexFormatBytes(VkFormat f) {
    const int v = static_cast<int>(f);
    if (v >= 107 && v <= 109) return 16;  // R32G32B32A32
    if (v >= 104 && v <= 106) return 12;  // R32G32B32
    if (v >= 101 && v <= 103) return 8;   // R32G32
    if (v >= 98 && v <= 100) return 4;    // R32
    if (v >= 91 && v <= 97) return 8;     // R16G16B16A16
    if (v >= 84 && v <= 90) return 6;     // R16G16B16
    if (v >= 77 && v <= 83) return 4;     // R16G16
    if (v >= 70 && v <= 76) return 2;     // R16
    if (v >= 37 && v <= 50) return 4;     // R8G8B8A8, B8G8R8A8
    if (v >= 51 && v <= 69) return 4;     // A8B8G8R8, A2R10G10B10, A2B10G10R10
    if (v >= 16 && v <= 22) return 2;     // R8G8
    if (v >= 9 && v <= 15) return 1;      // R8
    return 0;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                       const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* alloc,
                                                       VkPipeline* pipelines) {
    const VkResult r = Device(device)->CreateGraphicsPipelines(device, cache, count, infos, alloc, pipelines);
    Guard("CreateGraphicsPipelines", [&] {
        std::unique_lock lock(g_pipelineMu);
        for (uint32_t i = 0; i < count; ++i) {
            if (!pipelines[i]) continue;
            PipelineInfo p;
            if (const VkPipelineVertexInputStateCreateInfo* input = infos[i].pVertexInputState) {
                for (uint32_t b = 0; b < input->vertexBindingDescriptionCount; ++b) {
                    const VkVertexInputBindingDescription& d = input->pVertexBindingDescriptions[b];
                    if (d.binding >= efmidraw::kMaxBindings) continue;
                    p.strides[d.binding] = d.stride ? d.stride : 1;
                    if (d.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE || d.stride == 0) p.keepGame |= 1u << d.binding;
                }
            }
            p.handle = pipelines[i];
            if (const VkPipelineRasterizationStateCreateInfo* raster = infos[i].pRasterizationState) {
                p.cullMode = raster->cullMode;
                p.frontFace = raster->frontFace;
            }
            if (infos[i].pColorBlendState) p.colorTargets = infos[i].pColorBlendState->attachmentCount;
            if (infos[i].pDepthStencilState) p.depthWrite = infos[i].pDepthStencilState->depthWriteEnable != VK_FALSE;
            if (const VkPipelineVertexInputStateCreateInfo* input = infos[i].pVertexInputState) {
                for (uint32_t a = 0; a < input->vertexAttributeDescriptionCount; ++a) {
                    const VkVertexInputAttributeDescription& d = input->pVertexAttributeDescriptions[a];
                    if (d.binding >= efmidraw::kMaxBindings) continue;
                    const uint32_t size = VertexFormatBytes(d.format);
                    // Unknown format: all of the stride counts as read.
                    const uint32_t end = size ? d.offset + size : p.strides[d.binding];
                    if (p.extents[d.binding] != UINT32_MAX) p.extents[d.binding] = size ? std::max(p.extents[d.binding], end) : UINT32_MAX;
                }
                uint32_t read = 0;
                for (uint32_t a = 0; a < input->vertexAttributeDescriptionCount; ++a) {
                    if (input->pVertexAttributeDescriptions[a].binding < 32) read |= 1u << input->pVertexAttributeDescriptions[a].binding;
                }
                for (uint32_t b = 0; b < efmidraw::kMaxBindings; ++b) {
                    // The binding's attributes by offset; unknown with an unknown format or too many.
                    efmidraw::AttributeLayout& l = p.layouts[b];
                    bool known = true;
                    for (uint32_t a = 0; a < input->vertexAttributeDescriptionCount; ++a) {
                        const VkVertexInputAttributeDescription& d = input->pVertexAttributeDescriptions[a];
                        if (d.binding != b) continue;
                        const uint32_t size = VertexFormatBytes(d.format);
                        if (!size || l.count == 6) {
                            known = false;
                            break;
                        }
                        l.spans[l.count++] = {static_cast<uint16_t>(d.offset), static_cast<uint16_t>(size)};
                    }
                    if (!known) l.count = 0;
                    std::sort(l.spans, l.spans + l.count, [](const auto& x, const auto& y) { return x.offset < y.offset; });
                    if (p.extents[b] == UINT32_MAX) p.extents[b] = 0;
                    // Declared, but no attribute reads it: whatever the game binds there stays.
                    if (read && p.strides[b] && !(read & (1u << b))) p.keepGame |= 1u << b;
                }
                (*g_pipelineAttrs)[pipelines[i]].assign(input->pVertexAttributeDescriptions,
                                                        input->pVertexAttributeDescriptions + input->vertexAttributeDescriptionCount);
            }
            if (const VkPipelineDynamicStateCreateInfo* dyn = infos[i].pDynamicState) {
                for (uint32_t d = 0; d < dyn->dynamicStateCount; ++d) {
                    if (dyn->pDynamicStates[d] == VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE) p.dynamicStride = true;
                    if (dyn->pDynamicStates[d] == VK_DYNAMIC_STATE_CULL_MODE) p.dynamicCull = true;
                }
            }
            (*g_pipelines)[pipelines[i]] = p;
        }
    });
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* alloc) {
    {
        std::unique_lock lock(g_pipelineMu);
        g_pipelines->erase(pipeline);
        g_pipelineAttrs->erase(pipeline);
    }
    Device(device)->DestroyPipeline(device, pipeline, alloc);
}

VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline) {
    if (point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        Guard("CmdBindPipeline", [&] {
            CommandState* s = Command(cb);
            std::shared_lock lock(g_pipelineMu);
            const auto it = g_pipelines->find(pipeline);
            s->pipeline = it == g_pipelines->end() ? PipelineInfo{} : it->second;
        });
    }
    Device(cb)->CmdBindPipeline(cb, point, pipeline);
}

VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer* buffers,
                                                const VkDeviceSize* offsets) {
    CommandState* s = Command(cb);
    for (uint32_t i = 0; i < count && first + i < efmidraw::kMaxBindings; ++i) s->vb[first + i] = {buffers[i], offsets[i], 0, false};
    Device(cb)->CmdBindVertexBuffers(cb, first, count, buffers, offsets);
}

void BindVertexBuffers2Common(CommandState* s, uint32_t first, uint32_t count, const VkBuffer* buffers, const VkDeviceSize* offsets,
                              const VkDeviceSize* strides) {
    for (uint32_t i = 0; i < count && first + i < efmidraw::kMaxBindings; ++i) {
        s->vb[first + i] = {buffers[i], offsets[i], strides ? strides[i] : 0, strides != nullptr};
    }
}
VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer* buffers,
                                                 const VkDeviceSize* offsets, const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    BindVertexBuffers2Common(Command(cb), first, count, buffers, offsets, strides);
    Device(cb)->CmdBindVertexBuffers2(cb, first, count, buffers, offsets, sizes, strides);
}
VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers2EXT(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer* buffers,
                                                    const VkDeviceSize* offsets, const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    BindVertexBuffers2Common(Command(cb), first, count, buffers, offsets, strides);
    Device(cb)->CmdBindVertexBuffers2EXT(cb, first, count, buffers, offsets, sizes, strides);
}

VKAPI_ATTR void VKAPI_CALL CmdSetCullMode(VkCommandBuffer cb, VkCullModeFlags mode) {
    Command(cb)->dynamicCullMode = mode;
    Device(cb)->CmdSetCullMode(cb, mode);
}
VKAPI_ATTR void VKAPI_CALL CmdSetCullModeEXT(VkCommandBuffer cb, VkCullModeFlags mode) {
    Command(cb)->dynamicCullMode = mode;
    Device(cb)->CmdSetCullModeEXT(cb, mode);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, uint32_t count, uint32_t stride) {
    efmi::OnIndirectDraw();
    Device(cb)->CmdDrawIndexedIndirect(cb, buffer, offset, count, stride);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer,
                                                       VkDeviceSize countOffset, uint32_t maxCount, uint32_t stride) {
    efmi::OnIndirectDraw();
    Device(cb)->CmdDrawIndexedIndirectCount(cb, buffer, offset, countBuffer, countOffset, maxCount, stride);
}

// Secondary command buffers run inside a primary: their bone work goes with its submit.
VKAPI_ATTR void VKAPI_CALL CmdExecuteCommands(VkCommandBuffer cb, uint32_t count, const VkCommandBuffer* secondaries) {
    Guard("CmdExecuteCommands", [&] {
        efmibones::OnExecute(cb, count, secondaries);
        efmitex::OnExecute(cb, count, secondaries);
        for (uint32_t i = 0; i < count; ++i) {
            if (++Command(secondaries[i])->executions > 1) g_reexecuted.fetch_add(1, std::memory_order_relaxed);
        }
    });
    Device(cb)->CmdExecuteCommands(cb, count, secondaries);
}

// The CPU address of a buffer's first byte and its size, for efmibones.
const uint8_t* MappedBuffer(VkBuffer buffer, VkDeviceSize* size) {
    BufferInfo info;
    const uint8_t* mapped = nullptr;
    if (!FindBuffer(buffer, &info, &mapped) || !mapped) return nullptr;
    *size = info.size;
    return mapped;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    Guard("QueueSubmit", [&] {
        for (uint32_t i = 0; i < count; ++i) {
            for (uint32_t c = 0; c < submits[i].commandBufferCount; ++c) Submitted(submits[i].pCommandBuffers[c]);
        }
        thread_local std::vector<VkCommandBuffer> all;
        all.clear();
        for (uint32_t i = 0; i < count; ++i) all.insert(all.end(), submits[i].pCommandBuffers, submits[i].pCommandBuffers + submits[i].commandBufferCount);
        efmibones::OnSubmitted(all.data(), static_cast<uint32_t>(all.size()));
    });
    return Device(queue)->QueueSubmit(queue, count, submits, fence);
}

void Submit2Common(uint32_t count, const VkSubmitInfo2* submits) {
    Guard("QueueSubmit2", [&] {
        for (uint32_t i = 0; i < count; ++i) {
            for (uint32_t c = 0; c < submits[i].commandBufferInfoCount; ++c) Submitted(submits[i].pCommandBufferInfos[c].commandBuffer);
        }
        thread_local std::vector<VkCommandBuffer> all;
        all.clear();
        for (uint32_t i = 0; i < count; ++i) {
            for (uint32_t c = 0; c < submits[i].commandBufferInfoCount; ++c) all.push_back(submits[i].pCommandBufferInfos[c].commandBuffer);
        }
        efmibones::OnSubmitted(all.data(), static_cast<uint32_t>(all.size()));
    });
}
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence) {
    Submit2Common(count, submits);
    return Device(queue)->QueueSubmit2(queue, count, submits, fence);
}
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2KHR(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence) {
    Submit2Common(count, submits);
    return Device(queue)->QueueSubmit2KHR(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    Guard("QueuePresentKHR", [] {
        efmi::OnPresent();
        efmitex::OnPresent();
        efmibones::OnPresent();
    });
    return Device(queue)->QueuePresentKHR(queue, info);
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
    DeviceData* d = Device(device);
    PFN_vkDestroyDevice next = d->DestroyDevice;
    // The DeviceData stays: other threads may still be returning from calls into it.
    next(device, alloc);
}

// ---- Dispatch

// Our version of a device function, or null when we pass it through.
PFN_vkVoidFunction OwnDeviceFunction(const char* name) {
#define EFMI_HOOK(fn) \
    if (!strcmp(name, "vk" #fn)) return reinterpret_cast<PFN_vkVoidFunction>(&fn);
    EFMI_HOOK(DestroyDevice)
    EFMI_HOOK(CreateBuffer)
    EFMI_HOOK(DestroyBuffer)
    EFMI_HOOK(AllocateMemory)
    EFMI_HOOK(FreeMemory)
    EFMI_HOOK(MapMemory)
    EFMI_HOOK(UnmapMemory)
    EFMI_HOOK(MapMemory2)
    EFMI_HOOK(MapMemory2KHR)
    EFMI_HOOK(UnmapMemory2)
    EFMI_HOOK(UnmapMemory2KHR)
    EFMI_HOOK(BindBufferMemory)
    EFMI_HOOK(BindBufferMemory2)
    EFMI_HOOK(BindBufferMemory2KHR)
    EFMI_HOOK(BeginCommandBuffer)
    EFMI_HOOK(CmdCopyBuffer)
    EFMI_HOOK(CmdCopyBuffer2)
    EFMI_HOOK(CmdCopyBuffer2KHR)
    EFMI_HOOK(CmdBindIndexBuffer)
    EFMI_HOOK(CreateGraphicsPipelines)
    EFMI_HOOK(DestroyPipeline)
    EFMI_HOOK(CmdBindPipeline)
    EFMI_HOOK(CmdBindVertexBuffers)
    EFMI_HOOK(CmdBindVertexBuffers2)
    EFMI_HOOK(CmdBindVertexBuffers2EXT)
    EFMI_HOOK(CmdDrawIndexed)
    EFMI_HOOK(CreateImage)
    EFMI_HOOK(DestroyImage)
    EFMI_HOOK(CmdCopyBufferToImage)
    EFMI_HOOK(CmdCopyBufferToImage2)
    EFMI_HOOK(CmdCopyBufferToImage2KHR)
    EFMI_HOOK(UpdateDescriptorSets)
    EFMI_HOOK(UpdateDescriptorSetWithTemplate)
    EFMI_HOOK(UpdateDescriptorSetWithTemplateKHR)
    EFMI_HOOK(CmdPushDescriptorSetKHR)
    EFMI_HOOK(CmdPushDescriptorSetWithTemplateKHR)
    EFMI_HOOK(CmdBindDescriptorSets)
    EFMI_HOOK(CmdDrawIndexedIndirect)
    EFMI_HOOK(CmdDrawIndexedIndirectCount)
    EFMI_HOOK(QueueSubmit)
    EFMI_HOOK(QueueSubmit2)
    EFMI_HOOK(QueueSubmit2KHR)
    EFMI_HOOK(QueuePresentKHR)
    EFMI_HOOK(CmdExecuteCommands)
    EFMI_HOOK(CmdSetCullMode)
    EFMI_HOOK(CmdSetCullModeEXT)
    EFMI_HOOK(CreateImageView)
    EFMI_HOOK(DestroyImageView)
    EFMI_HOOK(CreateDescriptorSetLayout)
    EFMI_HOOK(CreatePipelineLayout)
    EFMI_HOOK(CreateDescriptorUpdateTemplate)
    EFMI_HOOK(CreateDescriptorUpdateTemplateKHR)
    EFMI_HOOK(DestroyDescriptorUpdateTemplate)
    EFMI_HOOK(DestroyDescriptorUpdateTemplateKHR)
    EFMI_HOOK(CreateCommandPool)
    EFMI_HOOK(AllocateCommandBuffers)
    EFMI_HOOK(CreateBufferView)
    EFMI_HOOK(DestroyBufferView)
#undef EFMI_HOOK
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&GetDeviceProcAddr);
    DeviceData* d = Device(device);
    if (!d) return nullptr;
    // Ours only when the next layer has the function: then the device supports it.
    const PFN_vkVoidFunction next = d->gdpa(device, name);
    if (!next || !Active()) return next;
    const PFN_vkVoidFunction own = OwnDeviceFunction(name);
    return own ? own : next;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info, const VkAllocationCallbacks* alloc, VkDevice* device) {
    auto* link = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(info->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO)) {
        link = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(link->pNext));
    }
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;  // For the next layer down.

    InstanceData* inst = Instance(physical);
    const auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst ? inst->instance : VK_NULL_HANDLE, "vkCreateDevice"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = create(physical, info, alloc, device);
    if (r != VK_SUCCESS) return r;

    auto* d = new DeviceData{};
    d->dispatchKey = DispatchKey(*device);
    d->device = *device;
    d->gdpa = gdpa;
    if (inst) inst->GetPhysicalDeviceMemoryProperties(physical, &d->memory);
#define EFMI_NEXT(fn) d->fn = reinterpret_cast<PFN_vk##fn>(gdpa(*device, "vk" #fn));
    EFMI_NEXT(DestroyDevice)
    EFMI_NEXT(CreateBuffer)
    EFMI_NEXT(DestroyBuffer)
    EFMI_NEXT(AllocateMemory)
    EFMI_NEXT(FreeMemory)
    EFMI_NEXT(MapMemory)
    EFMI_NEXT(UnmapMemory)
    EFMI_NEXT(BindBufferMemory)
    EFMI_NEXT(BindBufferMemory2)
    EFMI_NEXT(BeginCommandBuffer)
    EFMI_NEXT(CmdCopyBuffer)
    EFMI_NEXT(CmdCopyBuffer2)
    EFMI_NEXT(CmdBindIndexBuffer)
    EFMI_NEXT(CmdBindVertexBuffers)
    EFMI_NEXT(CmdBindVertexBuffers2)
    EFMI_NEXT(CreateGraphicsPipelines)
    EFMI_NEXT(DestroyPipeline)
    EFMI_NEXT(CmdBindPipeline)
    EFMI_NEXT(GetBufferMemoryRequirements)
    EFMI_NEXT(CmdDrawIndexed)
    EFMI_NEXT(CreateImage)
    EFMI_NEXT(DestroyImage)
    EFMI_NEXT(CmdCopyBufferToImage)
    EFMI_NEXT(CmdCopyBufferToImage2)
    EFMI_NEXT(UpdateDescriptorSets)
    EFMI_NEXT(UpdateDescriptorSetWithTemplate)
    EFMI_NEXT(CmdBindDescriptorSets)
    EFMI_NEXT(CmdDrawIndexedIndirect)
    EFMI_NEXT(CmdDrawIndexedIndirectCount)
    EFMI_NEXT(QueueSubmit)
    EFMI_NEXT(QueueSubmit2)
    EFMI_NEXT(QueuePresentKHR)
    EFMI_NEXT(CmdExecuteCommands)
    EFMI_NEXT(CreateImageView)
    EFMI_NEXT(DestroyImageView)
    EFMI_NEXT(GetImageMemoryRequirements)
    EFMI_NEXT(BindImageMemory)
    EFMI_NEXT(CmdPipelineBarrier)
    EFMI_NEXT(CreateDescriptorPool)
    EFMI_NEXT(ResetDescriptorPool)
    EFMI_NEXT(AllocateDescriptorSets)
    EFMI_NEXT(CreateDescriptorSetLayout)
    EFMI_NEXT(CreatePipelineLayout)
    EFMI_NEXT(CreateDescriptorUpdateTemplate)
    EFMI_NEXT(DestroyDescriptorUpdateTemplate)
    EFMI_NEXT(CreateCommandPool)
    EFMI_NEXT(AllocateCommandBuffers)
    d->CreateVkEvent = reinterpret_cast<PFN_vkCreateEvent>(gdpa(*device, "vkCreateEvent"));
    d->ResetVkEvent = reinterpret_cast<PFN_vkResetEvent>(gdpa(*device, "vkResetEvent"));
    d->CmdSetCullMode = reinterpret_cast<PFN_vkCmdSetCullMode>(gdpa(*device, "vkCmdSetCullMode"));
    d->CmdSetCullModeEXT = reinterpret_cast<PFN_vkCmdSetCullMode>(gdpa(*device, "vkCmdSetCullModeEXT"));
    EFMI_NEXT(CmdSetEvent)
    EFMI_NEXT(CreateBufferView)
    EFMI_NEXT(CmdCopyImageToBuffer)
    EFMI_NEXT(CmdCopyImage)
    EFMI_NEXT(DestroyBufferView)
    EFMI_NEXT(GetEventStatus)
#undef EFMI_NEXT
    d->CreateDescriptorUpdateTemplateKHR =
        reinterpret_cast<PFN_vkCreateDescriptorUpdateTemplate>(gdpa(*device, "vkCreateDescriptorUpdateTemplateKHR"));
    d->DestroyDescriptorUpdateTemplateKHR =
        reinterpret_cast<PFN_vkDestroyDescriptorUpdateTemplate>(gdpa(*device, "vkDestroyDescriptorUpdateTemplateKHR"));
    if (inst && inst->GetPhysicalDeviceQueueFamilyProperties) {
        uint32_t n = 0;
        inst->GetPhysicalDeviceQueueFamilyProperties(physical, &n, nullptr);
        std::vector<VkQueueFamilyProperties> props(n);
        inst->GetPhysicalDeviceQueueFamilyProperties(physical, &n, props.data());
        for (const VkQueueFamilyProperties& p : props) d->families.push_back(p.queueFlags);
    }
    d->BindBufferMemory2KHR = reinterpret_cast<PFN_vkBindBufferMemory2>(gdpa(*device, "vkBindBufferMemory2KHR"));
    d->MapMemory2 = reinterpret_cast<PFN_vkMapMemory2KHR>(gdpa(*device, "vkMapMemory2"));
    d->MapMemory2KHR = reinterpret_cast<PFN_vkMapMemory2KHR>(gdpa(*device, "vkMapMemory2KHR"));
    d->UnmapMemory2 = reinterpret_cast<PFN_vkUnmapMemory2KHR>(gdpa(*device, "vkUnmapMemory2"));
    d->UnmapMemory2KHR = reinterpret_cast<PFN_vkUnmapMemory2KHR>(gdpa(*device, "vkUnmapMemory2KHR"));
    d->CmdCopyBuffer2KHR = reinterpret_cast<PFN_vkCmdCopyBuffer2>(gdpa(*device, "vkCmdCopyBuffer2KHR"));
    d->QueueSubmit2KHR = reinterpret_cast<PFN_vkQueueSubmit2>(gdpa(*device, "vkQueueSubmit2KHR"));
    d->CmdBindVertexBuffers2EXT = reinterpret_cast<PFN_vkCmdBindVertexBuffers2>(gdpa(*device, "vkCmdBindVertexBuffers2EXT"));
    d->CmdCopyBufferToImage2KHR = reinterpret_cast<PFN_vkCmdCopyBufferToImage2>(gdpa(*device, "vkCmdCopyBufferToImage2KHR"));
    d->UpdateDescriptorSetWithTemplateKHR =
        reinterpret_cast<PFN_vkUpdateDescriptorSetWithTemplate>(gdpa(*device, "vkUpdateDescriptorSetWithTemplateKHR"));
    d->CmdPushDescriptorSetKHR = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(gdpa(*device, "vkCmdPushDescriptorSetKHR"));
    d->CmdPushDescriptorSetWithTemplateKHR =
        reinterpret_cast<PFN_vkCmdPushDescriptorSetWithTemplateKHR>(gdpa(*device, "vkCmdPushDescriptorSetWithTemplateKHR"));
    if (Active()) {
        efmidraw::DeviceFns fns;
        fns.device = *device;
        fns.memory = d->memory;
        fns.CreateBuffer = d->CreateBuffer;
        fns.GetBufferMemoryRequirements = d->GetBufferMemoryRequirements;
        fns.AllocateMemory = d->AllocateMemory;
        fns.BindBufferMemory = d->BindBufferMemory;
        fns.MapMemory = d->MapMemory;
        fns.CmdBindIndexBuffer = d->CmdBindIndexBuffer;
        fns.CmdBindVertexBuffers = d->CmdBindVertexBuffers;
        fns.CmdBindVertexBuffers2 = d->CmdBindVertexBuffers2 ? d->CmdBindVertexBuffers2 : d->CmdBindVertexBuffers2EXT;
        fns.CmdDrawIndexed = d->CmdDrawIndexed;
        efmidraw::SetDevice(fns);

        efmitex::DeviceFns tex;
        tex.device = *device;
        tex.memory = d->memory;
        tex.CreateImage = d->CreateImage;
        tex.GetImageMemoryRequirements = d->GetImageMemoryRequirements;
        tex.BindImageMemory = d->BindImageMemory;
        tex.CreateImageView = d->CreateImageView;
        tex.CreateBuffer = d->CreateBuffer;
        tex.GetBufferMemoryRequirements = d->GetBufferMemoryRequirements;
        tex.BindBufferMemory = d->BindBufferMemory;
        tex.AllocateMemory = d->AllocateMemory;
        tex.MapMemory = d->MapMemory;
        tex.CmdPipelineBarrier = d->CmdPipelineBarrier;
        tex.CmdCopyBufferToImage = d->CmdCopyBufferToImage;
        tex.CreateDescriptorPool = d->CreateDescriptorPool;
        tex.ResetDescriptorPool = d->ResetDescriptorPool;
        tex.AllocateDescriptorSets = d->AllocateDescriptorSets;
        tex.UpdateDescriptorSetWithTemplate = d->UpdateDescriptorSetWithTemplate ? d->UpdateDescriptorSetWithTemplate : d->UpdateDescriptorSetWithTemplateKHR;
        tex.CmdBindDescriptorSets = d->CmdBindDescriptorSets;
        tex.CreateVkEvent = d->CreateVkEvent;
        tex.CmdSetEvent = d->CmdSetEvent;
        tex.GetEventStatus = d->GetEventStatus;
        tex.CmdCopyImageToBuffer = d->CmdCopyImageToBuffer;
        tex.DestroyBuffer = d->DestroyBuffer;
        tex.FreeMemory = d->FreeMemory;
        tex.ResetVkEvent = d->ResetVkEvent;
        tex.CmdCopyImage = d->CmdCopyImage;
        efmitex::SetDevice(tex);
        efmitex::Lookups lookups;
        lookups.Contents = &SetContents;
        lookups.Updates = &SetUpdates;
        lookups.TextureHashOfView = &TextureHashOfView;
        lookups.ImageOfView = &ImageOfView;
        lookups.TextureHashOfImage = &TextureHashOfImage;
        lookups.OnReadback = &OnReadback;
        lookups.OnReadbackMip = &OnReadbackMip;
        efmitex::SetLookups(lookups);

        efmibones::DeviceFns bones;
        bones.device = *device;
        bones.memory = d->memory;
        bones.CreateBuffer = d->CreateBuffer;
        bones.GetBufferMemoryRequirements = d->GetBufferMemoryRequirements;
        bones.AllocateMemory = d->AllocateMemory;
        bones.BindBufferMemory = d->BindBufferMemory;
        bones.MapMemory = d->MapMemory;
        efmibones::SetDevice(bones);
        efmibones::Lookups boneLookups;
        boneLookups.Contents = &SetContents;
        boneLookups.Mapped = &MappedBuffer;
        efmibones::SetLookups(boneLookups);
        efmidraw::OnDeviceReady();
    }
    {
        std::unique_lock lock(g_dispatchMu);
        (*g_devices)[DispatchKey(*device)] = d;
    }
    if (Active()) LogInfo("Vulkan device created; %u memory types", d->memory.memoryTypeCount);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
    InstanceData* inst = Instance(instance);
    if (inst) inst->DestroyInstance(instance, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* alloc, VkInstance* instance) {
    auto* link = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(info->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO)) {
        link = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(link->pNext));
    }
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    const auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = create(info, alloc, instance);
    if (r != VK_SUCCESS) return r;

    InstanceData data{};
    data.instance = *instance;
    data.gipa = gipa;
    data.DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(*instance, "vkDestroyInstance"));
    data.GetPhysicalDeviceMemoryProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(*instance, "vkGetPhysicalDeviceMemoryProperties"));
    data.GetPhysicalDeviceQueueFamilyProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(*instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    {
        std::unique_lock lock(g_dispatchMu);
        (*g_instances)[DispatchKey(*instance)] = data;
    }
    if (Active()) StartBackground();
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name) {
#define EFMI_OWN(fn) \
    if (!strcmp(name, "vk" #fn)) return reinterpret_cast<PFN_vkVoidFunction>(&fn);
    EFMI_OWN(GetInstanceProcAddr)
    EFMI_OWN(CreateInstance)
    EFMI_OWN(DestroyInstance)
    EFMI_OWN(CreateDevice)
    EFMI_OWN(GetDeviceProcAddr)
#undef EFMI_OWN
    InstanceData* inst = instance ? Instance(instance) : nullptr;
    if (!inst) return nullptr;
    // Device functions asked for through the instance: ours as well, so a game that
    // gets them before it has a device still goes through the layer.
    const PFN_vkVoidFunction next = inst->gipa(instance, name);
    if (!next || !Active()) return next;
    const PFN_vkVoidFunction own = OwnDeviceFunction(name);
    return own ? own : next;
}

}  // namespace
}  // namespace efmivulkan

// The loader's entry point for layers (interface version 2), exported by efmivulkan.def.
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (v->loaderLayerInterfaceVersion >= 2) {
        v->pfnGetInstanceProcAddr = &efmivulkan::GetInstanceProcAddr;
        v->pfnGetDeviceProcAddr = &efmivulkan::GetDeviceProcAddr;
        v->pfnGetPhysicalDeviceProcAddr = nullptr;
        v->loaderLayerInterfaceVersion = 2;
    }
    return VK_SUCCESS;
}
