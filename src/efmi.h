// EFMI's view of the game's draws, independent of the graphics API: which indexed
// draws belong to a mod's component (index region hash + index count), and F10's
// one-frame capture with its reports.
#pragma once
#include <cstdint>

namespace efmivulkan {
class ModIndex;
}

namespace efmivulkan::efmi {

// Loads the mods' hashes on the job thread. Null until loaded; an index never changes.
void LoadModIndex();
const ModIndex* GetModIndex();

// The index data a draw reads. mapped: the bytes in place when the buffer's memory is
// host-visible and mapped, else null (then only uploads seen earlier can be hashed).
struct IndexedDraw {
    uint64_t key;  // ibregion::BufferKey of the binding offset.
    uint32_t indexBytes;
    uint32_t indexCount, instanceCount, firstIndex, firstInstance;
    int32_t vertexOffset;
    const uint8_t* mapped;
};

// Every indexed draw, from the render threads. Cheap unless the index count is one a
// mod uses, or a capture is running. Returns the region hash when a mod has it, else 0.
uint32_t OnIndexedDraw(const IndexedDraw& d);
void OnIndirectDraw();
void OnPresent();

void RequestCapture();  // F10.
void LogStats();

}  // namespace efmivulkan::efmi
