// Milestone 2a: draws a mod's mesh instead of a matched component.
//
//   mod index loaded -> every EFMI mod parsed (efmimod) -> hash to component
//   first match of a mod -> its buffers loaded into GPU memory (job thread)
//   matched draw -> checks pass -> mod's index and vertex buffers bound, its draws issued,
//                   the game's bindings put back, the game's draw skipped
//
// Checks, each logged once when it says no and the game's draw goes ahead instead:
//   the component needs EFMI's merged skeleton (gpu_posed): milestone 2b
//   the draw has several instances, which need spatial identification: milestone 2d
//   a per-vertex binding the pipeline reads has another stride than the mod's buffer
//   the pipeline reads a per-vertex binding the mod has no buffer for
// Per-instance bindings keep the game's buffers.
//   a draw would read past the end of a mod buffer (worked out when it loads)
// Textures (milestone 2c): a mod draw's sets show the mod's textures in place of the game
// textures they name (efmitex.h), once those have uploaded.
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>

#include "efmitex.h"

namespace efmivulkan::efmidraw {

constexpr uint32_t kMaxBindings = 16;

// The next layer's functions the runtime calls itself, set when the device is created.
struct DeviceFns {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer = nullptr;
    PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers = nullptr;
    PFN_vkCmdBindVertexBuffers2 CmdBindVertexBuffers2 = nullptr;  // May be null.
    PFN_vkCmdDrawIndexed CmdDrawIndexed = nullptr;
};
void SetDevice(const DeviceFns& fns);

// Parses every mod with EFMI components. Job thread, after the mod index loads.
void LoadMods();

// What a command buffer has bound when it draws.
struct VertexBinding {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize stride = 0;  // Only when bound with vkCmdBindVertexBuffers2 and a stride.
    bool strideGiven = false;
};
// The attributes a pipeline reads from one vertex binding, by offset (count 0: unknown).
struct AttributeLayout {
    uint8_t count = 0;
    struct Span {
        uint16_t offset, size;
    } spans[6];
};

struct BindState {
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceSize indexOffset = 0;
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;
    const VertexBinding* vb = nullptr;  // kMaxBindings.
    const uint32_t* pipelineStrides = nullptr;  // kMaxBindings, 0: the pipeline reads no such binding.
    bool dynamicStride = false;  // The pipeline takes strides from vkCmdBindVertexBuffers2.
    // Bindings that are not mesh data, so the game's buffers stay bound: read once per
    // instance, or with stride 0 (every vertex reads the same element).
    uint32_t keepGame = 0;
    const efmitex::BoundSet* sets = nullptr;  // efmitex::kMaxSets, bound for graphics.
    // The pass the draw is in, from its pipeline.
    uint32_t cullMode = 0, frontFace = 0, colorTargets = 0;
    bool depthWrite = false;
    uint64_t pipeline = 0;
    // kMaxBindings: where the last attribute the pipeline reads from a binding ends; the
    // bytes past it are not read (padding). Null or 0: as the stride.
    const uint32_t* pipelineExtents = nullptr;
    const AttributeLayout* pipelineLayouts = nullptr;  // kMaxBindings.
};

struct Draw {
    uint32_t hash, indexCount, instanceCount, firstInstance;
    uint32_t firstIndex = 0;
    int32_t vertexOffset = 0;
};

// True when it drew the mod, and the game's draw must be skipped.
bool TryReplace(VkCommandBuffer cb, const Draw& draw, const BindState& state);

void OnPresent();
// After every part (efmitex, efmibones) has the device: mods load from then on.
void OnDeviceReady();
void LogStats();
void PollKeys();  // From the input thread.
bool ToggleEnabled();

}  // namespace efmivulkan::efmidraw
