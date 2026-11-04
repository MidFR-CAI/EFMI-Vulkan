// Milestone 2c: mod textures in mod draws.
//
// Loading: each mod texture (DDS) becomes an image with a view. Its pixels go up from a
// staging buffer by copies recorded at the start of one of the game's own primary
// graphics command buffers, as no queue is ours. An event set after the copies tells
// when the GPU has done them: the texture is ready from then on.
//
// Swapping: Unity writes descriptor sets with update templates. The layer keeps the data
// each set was last written with (layer.cpp). For a mod draw, a set holding a game
// texture the mod replaces is written again, from the same template and data, into a
// set of ours with the mod texture's view in its place, bound for the mod's draws, and
// the game's set bound again after. Our sets come from a ring of pools, one per frame in
// flight, each reset when its frame comes round again.
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace efmivulkan::efmitex {

constexpr uint32_t kMaxSets = 8;
constexpr uint32_t kMaxDynamic = 16;

struct DeviceFns {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
    PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
    PFN_vkResetDescriptorPool ResetDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSetWithTemplate UpdateDescriptorSetWithTemplate = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkCreateEvent CreateVkEvent = nullptr;  // Not CreateEvent: a Windows macro.
    PFN_vkCmdSetEvent CmdSetEvent = nullptr;
    PFN_vkGetEventStatus GetEventStatus = nullptr;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkResetEvent ResetVkEvent = nullptr;
    PFN_vkCmdCopyImage CmdCopyImage = nullptr;
};
void SetDevice(const DeviceFns& fns);

// A mod's textures, loaded on the job thread. Returns an id per texture, -1 where a
// file could not be used; the ids index Ready() and View().
std::vector<int> Load(const std::string& mod, const std::vector<std::wstring>& files);
bool Ready(int id);
VkImageView View(int id);

// From the layer: a primary command buffer of a graphics queue family was begun, so
// pending uploads can go at its start; and command buffers were submitted.
void OnBegin(VkCommandBuffer cb);
void OnSubmitted(VkCommandBuffer cb);
// Secondaries run by a primary: their copies are checked when it is submitted.
void OnExecute(VkCommandBuffer primary, uint32_t count, const VkCommandBuffer* secondaries);
void OnPresent();

// A set as the layer last saw it written.
struct ImageEntry {
    uint32_t offset, stride, count;  // In the template's data.
};
struct SetContents {
    VkDescriptorUpdateTemplate tmpl = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    bool updateAfterBind = false;
    std::vector<ImageEntry> images;
    std::vector<VkDescriptorUpdateTemplateEntry> entries;  // All of them.
    std::vector<uint8_t> data;
    uint64_t updates = 0;  // The set's update count when read.
};

inline bool IsDynamic(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}
inline bool IsBuffer(VkDescriptorType t) {
    return IsDynamic(t) || t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
}
// Which of a set's dynamic offsets a descriptor takes: they go by binding, then array
// element. -1 when it takes none.
inline int DynamicIndex(const std::vector<VkDescriptorUpdateTemplateEntry>& entries, uint32_t binding, uint32_t element) {
    int before = 0;
    bool found = false;
    for (const auto& e : entries) {
        if (!IsDynamic(e.descriptorType)) continue;
        for (uint32_t k = 0; k < e.descriptorCount; ++k) {
            const uint32_t b = e.dstBinding, el = e.dstArrayElement + k;
            if (b == binding && el == element) found = true;
            if (b < binding || (b == binding && el < element)) ++before;
        }
    }
    return found ? before : -1;
}

// A buffer descriptor of a bound set pointed somewhere else for a mod's draws.
struct BufferPatch {
    uint32_t set = 0, binding = 0, element = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, range = 0;
    uint32_t dynamicOffset = 0;  // For dynamic descriptors.
};
// The game's sets bound for graphics when a mod draws.
struct BoundSet {
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    uint32_t dynamicCount = 0;
    uint32_t dynamic[kMaxDynamic]{};
};

// A game image, as the layer knows it.
struct ImageDesc {
    uint64_t id = 0;  // The layer's id, for textures::OnImageUpload.
    VkImage image = VK_NULL_HANDLE;
    uint32_t format = 0, width = 0, height = 0, mips = 0;
    bool readable = false;  // A texture the layer can copy from (TRANSFER_SRC, not a render target).
};

// The layer's lookups.
struct Lookups {
    bool (*Contents)(VkDescriptorSet set, SetContents* out) = nullptr;
    uint64_t (*Updates)(VkDescriptorSet set) = nullptr;  // How many times it has been updated.
    uint32_t (*TextureHashOfView)(VkImageView view) = nullptr;  // The mod hash it matched, or 0.
    bool (*ImageOfView)(VkImageView view, ImageDesc* out) = nullptr;
    uint32_t (*TextureHashOfImage)(uint64_t imageId) = nullptr;
    // A read-back mip 0, to hash as an upload would be.
    void (*OnReadback)(const ImageDesc& image, const uint8_t* mip0, uint64_t bytes) = nullptr;
    void (*OnReadbackMip)(const ImageDesc& image, uint32_t mip, const uint8_t* data, uint64_t bytes) = nullptr;  // Mips 1 and on.
};
void SetLookups(const Lookups& l);

// Readback: game textures a mod draw binds that have no hash yet (their pixels went up from
// memory the CPU cannot read, or by a copy the layer does not see). Mip 0 is copied to
// memory the CPU reads at the start of one of the game's graphics command buffers, as mod
// textures go up, and hashed when the GPU is done: the hash 3DMigoto made from the pixels
// the DX11 game created the texture with. One that matches nothing is tried again later, in
// case its full-size mip had not streamed in yet.
void RequestReadback(const ImageDesc& image, VkImageLayout layout);
void OnImageDestroyed(uint64_t imageId);  // From the layer, before the image goes.
// In place: the mod's pixels copied into the game texture it replaces (see efmitex.cpp).
void RequestInPlace(const ImageDesc& image, VkImageLayout layout, int texture);
void OnGameUpload(uint64_t imageId);  // From the layer: the game uploaded into an image.
void LogStats();

// Alt+M: every slot a mod texture would go into gets solid magenta instead, to see on screen
// which surfaces the swaps reach. Returns whether it is now on.
bool ToggleMagenta();


// Binds sets of ours where the bound sets hold game textures in replace (game texture
// hash -> texture id), or buffers the patches point elsewhere. Returns which set indices
// it bound, for Restore.
//
// Sticky swaps: a mod texture that replaced the texture in a slot (per mod component, pass
// and slot) also replaces an unidentified one there later, of the same format: Unity makes
// new images when it streams another size in, and those need identifying again.
struct BindOptions {
    uint64_t drawKey = 0;   // The mod component.
    uint64_t pipeline = 0;  // The pass.
};
uint32_t Bind(VkCommandBuffer cb, const BoundSet* sets, const std::map<uint32_t, int>& replace, const BufferPatch* patches = nullptr,
              uint32_t patchCount = 0, const BindOptions* options = nullptr);
void Restore(VkCommandBuffer cb, const BoundSet* sets, uint32_t bound);

}  // namespace efmivulkan::efmitex
