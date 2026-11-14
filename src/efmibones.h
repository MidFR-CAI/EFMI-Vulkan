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
#include <string>
#include <vector>

#include "efmitex.h"
#include "spirv.h"

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
// The skeleton as of one frame: imports apply when recorded, into their frame's version; a
// mod draw is filled at submit from the version of the frame it was recorded in. So it never
// gets bones a later frame imported while it waited to be submitted (game command buffers
// are submitted up to 3 frames after recording), nor bones older than its own frame's.
constexpr uint32_t kVersions = 8;
struct SkeletonVersion {
    uint64_t frame = UINT64_MAX;  // UINT64_MAX: unused.
    std::vector<float> current, previous;  // 12 floats a bone: this frame's and last frame's.
    std::vector<uint8_t> imported;         // Per component: imported in this frame itself.
};
struct SkeletonInstance {
    std::array<SkeletonVersion, kVersions> versions;
    size_t floats = 0;  // 12 a bone.
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
    std::string name;         // The mod's, for the log.
    void Init(uint32_t boneCount, std::vector<Component> c);
};

// Where a draw reads its bones, found in its bound sets.
struct BoneBinding {
    uint32_t configSet = 0, configBinding = 0, configElement = 0;
    const uint8_t* config = nullptr;  // Instance 0's rows, mapped.
    uint64_t configAvailable = 0;     // Bytes mapped from config on (at most the block's 64 KB).
    uint32_t bonesSet = 0, bonesBinding = 0, bonesElement = 0;
    const uint8_t* bones = nullptr;   // Row 0, mapped.
    uint64_t boneRows = 0;
};
// vs: the buffers the draw's vertex shader declares (spirv::Resources); only those are
// candidates. Null or empty: every bound buffer is (the largest storage buffer is the bones),
// which picked the wrong buffer for pipelines with more than one.
bool FindBoneBinding(const efmitex::BoundSet* sets, BoneBinding* out, const std::vector<spirv::Resource>* vs = nullptr);

// A modded instance of a component (CommandListMergedSkeleton_Apply): its bones are imported
// into the slot's skeleton once a frame, again for a better LoD; copied when the command
// buffer is submitted, in recorded order, as EFMI's compute shaders run in GPU order.
// config: that instance's 16 rows.
void Import(VkCommandBuffer cb, Skeleton* s, uint32_t component, int lod, const BoneBinding& b, uint32_t slot, const uint8_t* config);

// Whether a draw keeps its bones the usual way (EFMI's $enum_bones_source_vs_t0: a 16-row
// instance config, row 5 into the rolling bone buffer): a sane transform in rows 0 to 3, and
// bones that are a pose (translations within 50 m). Effect passes keep config and bones
// otherwise (EFMI's constant-buffer path); read the usual way they are garbage, and the mod's
// mesh is thrown across the screen. EFMI leaves a draw it cannot pose to the game.
bool UsualBones(const BoneBinding& b, uint32_t sampleBones, std::string* why = nullptr);

// A new spatial slot that is an object the mod knows, moved (a party swap, a dodge): it starts
// with that slot's bones, not zeros (bones are in the object's own space), and imports its own
// as soon as it is drawn.
void CopyInstance(Skeleton* s, uint32_t from, uint32_t to);

// The mod's vertices of a draw, for a check at fill time: a sample skinned on the CPU as the
// vertex shader would, with the bones the draw gets. Null pointers: no check.
struct SkinCheck {
    const uint8_t* positions = nullptr;  // R32G32B32_SFLOAT at offset 0.
    uint32_t positionStride = 0;
    const uint8_t* blend = nullptr;  // Weights (16-bit UNORM or float), then palette indices (8 or 32-bit).
    uint32_t blendStride = 0;
    uint64_t vertices = 0;
};

// A mod draw with a palette (its local bone -> merged bone): fills patches[2] for
// efmitex::Bind. False when the ring is full this frame.
bool Redirect(VkCommandBuffer cb, Skeleton* s, uint32_t component, int lod, const std::vector<uint16_t>* palette, const BoneBinding& b,
              uint32_t slot, const uint8_t* config, efmitex::BufferPatch patches[2], const SkinCheck* check = nullptr);

// From the layer.
void OnBegin(VkCommandBuffer cb);
void OnExecute(VkCommandBuffer primary, uint32_t count, const VkCommandBuffer* secondaries);
// The command buffers of one vkQueueSubmit: their imports and fills run in recorded order.
void OnSubmitted(const VkCommandBuffer* cbs, uint32_t count);
void OnPresent();
void LogStats();

// Alt+B, to compare how mod draws get their bones and config (logged, and shown by beeps).
// Three switches, as bits of the mode (0 to 7):
//   1  the config a mod draw gets: as recorded (off), as it is at submit (on)
//   2  the bones a mod draw gets: of the frame it was recorded in (off), the newest (on)
//   4  when the game's bones are read: as the draw is recorded (off), at submit (on)
// Returns the new mode.
int CycleMode();


}  // namespace efmivulkan::efmibones
