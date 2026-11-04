// CPU versions of WWMI's skeleton compute shaders (SkeletonMerger, SkeletonRemapper
// and BlendRemapper). A bone is 3 float4 rows. Mods address bones through a merged
// skeleton, because UE's per-component bone lists are numbered separately.
#pragma once
#include <cstddef>
#include <cstdint>

namespace efmivulkan {

constexpr int kRowsPerBone = 3;
constexpr int kMaxMergedBones = 512;   // WWMI: array = 1536 float4.
constexpr int kMaxBonesPerDraw = 256;  // Blend indices are 8-bit.
constexpr int kRemapBoneCount = 512;   // WWMI: $blend_remap_bone_count.

using Row = float[4];

// DX11 finds bone buffers by hash; DX12 root CBVs carry no such identity, so this
// looks at the data. rows: at least count float4s.
bool LooksLikeBones(const Row* rows, int count);

// SkeletonMerger: copies bones to vgOffset.. from a game buffer, skipping empty ones.
void MergeBones(const Row* game, Row* merged, int vgOffset, int vgCount);

// SkeletonRemapper: first vgCount bones become merged[forward[remap * 512 + i]].
// The rest stay as in merged, as WWMI starts from a copy of it.
void RemapBones(const Row* merged, const uint16_t* forward, size_t forwardCount, int remapId, int vgCount, Row* out);

// BlendRemapper: rewrites each vertex's 8-bit bone ids. blend is the mod's Blend.buf
// [ids][weights], vertexVg its BlendRemapVertexVG.buf with the full 16-bit ids.
void RemapBlend(uint8_t* blend, size_t blendBytes, uint32_t stride, const uint16_t* vertexVg, size_t vertexVgCount,
                const uint16_t* reverse, size_t reverseCount, int remapId);

}  // namespace efmivulkan
