// The game's shape key pass, done on the CPU for a mod's vertices. On DX11 WWMI
// feeds the mod's entries to the game's own compute shaders (ShapeKeyLoader, then
// ShapeKeyMultiplier); here the same sums are computed from the game's weights:
//
//   entries of key k in a batch: [base + offsets[k], base + offsets[k + 1])
//   each entry: a vertex id and 6 halfs, position xyz then normal xyz
//   vertex += sum of entry * weight; normals are divided by the summed weight above 1
//
// The result is what the game binds as vb6: 6 floats per vertex.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace efmivulkan::shapekeys {

constexpr int kKeysPerBatch = 128;
constexpr int kControlRows = 66;  // uint4 rows of the game's constant buffer.
constexpr int kFloatsPerVertex = 6;

// The game's constant buffer: offsets in rows 0-31, weights in rows 32-63,
// scales in row 64, and the batch's first entry in row 65.y.
inline uint32_t Checksum(const uint32_t* control) { return control[0] + control[1] + control[2] + control[3]; }
inline const float* Weights(const uint32_t* control) { return reinterpret_cast<const float*>(control + 32 * 4); }
inline uint32_t FirstEntry(const uint32_t* control) { return control[65 * 4 + 1]; }

// Sane offsets: they never decrease. Guards against a constant buffer that only
// happens to have the right checksum.
bool LooksLikeControl(const uint32_t* control);

struct Batch {
    uint32_t customOffset;   // Where this batch's entries start in the mod's buffers.
    const float* weights;    // kKeysPerBatch, or null when the game has not run it yet.
};

float HalfToFloat(uint16_t h);

// offsets: kKeysPerBatch per batch. out is resized to vertices * kFloatsPerVertex.
void Apply(const uint32_t* offsets, size_t offsetCount, const uint32_t* ids, const uint16_t* deltas, size_t entries,
           const Batch* batches, size_t batchCount, uint32_t vertices, std::vector<float>* out);

}  // namespace efmivulkan::shapekeys
