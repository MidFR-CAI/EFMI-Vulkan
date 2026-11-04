// Milestone 2b: EFMI's merged skeleton, on Vulkan.
//
// How Endfield skins (the same on DX11 and Vulkan): a draw's vertex shader reads bone
// matrices, 3 rows of 16 bytes per bone, from a big rolling storage buffer. Where its bones
// start is in a 16-row instance config (one per instance, in a 64 KB uniform buffer): row 5
// holds the row of this frame's bones (x) and of last frame's (y), for motion vectors; bone
// i is at x + 3 + 3i. Offsets below kStaticRows mean the draw has no skeleton.
//
// A gpu_posed mod numbers its bone indices in a skeleton merged from all its components:
// component c owns bones [offset, offset + count). EFMI copies each component's bones into
// a merged buffer with compute shaders, and points row 5 there. Vulkan has no compute or
// copies inside a render pass, where the draws are, so here:
//
//   game draw of a component    Import: where its bones are is noted; at submit, when the
//                               game has written them, they are copied into the mod's
//                               merged skeleton (CPU memory), LoD remaps applied.
//   mod draw                    Redirect: a block of a ring buffer of ours; at submit it
//                               gets the palette's bones (this and last frame) and a copy
//                               of the game's instance config with row 5 pointing at them.
//                               The draw's sets are bound with the bone buffer and the
//                               config pointing at the ring (efmitex::Bind patches).
//
// The game's pipelines read 8-bit bone indices, the merged skeleton has more than 256 bones:
// each mod vertex buffer with bone indices gets its own palette of at most 256 bones when it
// loads (efmidraw), its indices made 8-bit indices into it.
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <vector>

#include "efmitex.h"

namespace efmivulkan::efmibones {

constexpr uint32_t kMaxLods = 4;
constexpr uint32_t kStaticRows = 2 * 256 * 3;
constexpr uint32_t kConfigBytes = 16 * 16;  // One instance's config.
constexpr uint32_t kConfigRange = 65536;    // The config buffer the shaders declare: 4096 rows.
constexpr uint32_t kBoneOffsetsRow = 5;

struct DeviceFns {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
};
void SetDevice(const DeviceFns& fns);

struct Lookups {
    bool (*Contents)(VkDescriptorSet set, efmitex::SetContents* out) = nullptr;
    const uint8_t* (*Mapped)(VkBuffer buffer, VkDeviceSize* size) = nullptr;  // Null: the CPU cannot read it.
};
void SetLookups(const Lookups& l);

// A mod's merged skeleton.
struct Component {
    uint32_t offset = 0, count = 0;
    std::array<std::vector<uint16_t>, kMaxLods> lodRemap;  // Empty: none. Merged-local bone -> the LoD's bone.
};
// EFMI's merged skeleton buffer, one region per spatial-identification slot (the object's
// "Spatial ID", MergedSkeleton.ini: $merged_skeleton_instance_id). Bones not imported yet
// are zero, as EFMI's buffer is outside its initializer's implicit bones.
constexpr uint32_t kMaxInstances = 32;  // $cfg_spatial_max_instance_count.
struct SkeletonInstance {
    std::vector<float> current, previous;  // 12 floats a bone: this frame's and last frame's.
    // $PoolInput_MergedSkeleton_Instance_UpdateFrame and _LodLevel, per component.
    std::vector<uint64_t> updateFrame;
    std::vector<int> lodLevel;
};
struct Skeleton {
    uint32_t bones = 0;
    std::vector<Component> components;
    std::vector<uint16_t> boneComponent;  // The component each bone comes from; 0xffff: none.
    float meshScale = 1.0f;               // $\EFMIv1\custom_mesh_scale: the bones are scaled by it.
    std::mutex mu;
    SkeletonInstance instances[kMaxInstances];
    bool fillLogged = false;  // The first fill is logged, to check the bones that arrive.
    void Init(uint32_t boneCount, std::vector<Component> c);
};

// Where a draw reads its bones, found in its bound sets.
struct BoneBinding {
    uint32_t configSet = 0, configBinding = 0, configElement = 0;
    const uint8_t* config = nullptr;  // Instance 0's rows, mapped.
    uint32_t bonesSet = 0, bonesBinding = 0, bonesElement = 0;
    const uint8_t* bones = nullptr;   // Row 0, mapped.
    uint64_t boneRows = 0;
};
bool FindBoneBinding(const efmitex::BoundSet* sets, BoneBinding* out);

// A modded instance of a component (CommandListMergedSkeleton_Apply): its bones are imported
// into the slot's skeleton once a frame, again for a better LoD; copied when the command
// buffer is submitted, in recorded order, as EFMI's compute shaders run in GPU order.
// config: that instance's 16 rows.
void Import(VkCommandBuffer cb, Skeleton* s, uint32_t component, int lod, const BoneBinding& b, uint32_t slot, const uint8_t* config);

// A mod draw with a palette (its local bone -> merged bone): fills patches[2] for
// efmitex::Bind. False when the ring is full this frame.
bool Redirect(VkCommandBuffer cb, Skeleton* s, uint32_t component, int lod, const std::vector<uint16_t>* palette, const BoneBinding& b,
              uint32_t slot, const uint8_t* config, efmitex::BufferPatch patches[2]);

// From the layer.
void OnBegin(VkCommandBuffer cb);
void OnExecute(VkCommandBuffer primary, uint32_t count, const VkCommandBuffer* secondaries);
// The command buffers of one vkQueueSubmit: their imports and fills run in recorded order.
void OnSubmitted(const VkCommandBuffer* cbs, uint32_t count);
void OnPresent();
void LogStats();


}  // namespace efmivulkan::efmibones
