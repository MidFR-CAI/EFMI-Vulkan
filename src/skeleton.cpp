#include "skeleton.h"

#include <cmath>
#include <cstring>

namespace efmivulkan {
namespace {

bool RowEmpty(const Row& r) { return r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 0; }

float Dot3(const Row& a, const Row& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

}  // namespace

bool LooksLikeBones(const Row* rows, int count) {
    // The first used bone decides: it must be a rotation and scale, meaning rows of
    // similar length at right angles. Unused bones are all zero.
    constexpr float kMinLength = 0.1f, kMaxLength = 10.0f, kMaxSkew = 0.25f;
    for (int bone = 0; (bone + 1) * kRowsPerBone <= count; ++bone) {
        const Row* b = rows + bone * kRowsPerBone;
        if (RowEmpty(b[0]) && RowEmpty(b[1]) && RowEmpty(b[2])) continue;
        const float len[3] = {std::sqrt(Dot3(b[0], b[0])), std::sqrt(Dot3(b[1], b[1])), std::sqrt(Dot3(b[2], b[2]))};
        for (float l : len) {
            if (!(l >= kMinLength && l <= kMaxLength)) return false;
        }
        return std::fabs(Dot3(b[0], b[1])) <= kMaxSkew * len[0] * len[1] &&
               std::fabs(Dot3(b[0], b[2])) <= kMaxSkew * len[0] * len[2] &&
               std::fabs(Dot3(b[1], b[2])) <= kMaxSkew * len[1] * len[2];
    }
    return false;
}

void MergeBones(const Row* game, Row* merged, int vgOffset, int vgCount) {
    for (int i = 0; i < vgCount && vgOffset + i < kMaxMergedBones; ++i) {
        const Row* from = game + i * kRowsPerBone;
        if (RowEmpty(from[0]) && RowEmpty(from[1]) && RowEmpty(from[2])) continue;
        std::memcpy(merged + (vgOffset + i) * kRowsPerBone, from, sizeof(Row) * kRowsPerBone);
    }
}

void RemapBones(const Row* merged, const uint16_t* forward, size_t forwardCount, int remapId, int vgCount, Row* out) {
    std::memcpy(out, merged, sizeof(Row) * kMaxMergedBones * kRowsPerBone);
    for (int i = 0; i < vgCount && i < kMaxBonesPerDraw; ++i) {
        const size_t at = static_cast<size_t>(remapId) * kRemapBoneCount + i;
        if (at >= forwardCount || forward[at] >= kMaxMergedBones) continue;
        std::memcpy(out + i * kRowsPerBone, merged + forward[at] * kRowsPerBone, sizeof(Row) * kRowsPerBone);
    }
}

void RemapBlend(uint8_t* blend, size_t blendBytes, uint32_t stride, const uint16_t* vertexVg, size_t vertexVgCount,
                const uint16_t* reverse, size_t reverseCount, int remapId) {
    const uint32_t weights = stride / 2;
    const size_t vertices = blendBytes / stride;
    for (size_t v = 0; v < vertices; ++v) {
        for (uint32_t i = 0; i < weights; ++i) {
            const size_t vgAt = v * weights + i;
            if (vgAt >= vertexVgCount) return;
            const size_t mapAt = static_cast<size_t>(remapId) * kRemapBoneCount + vertexVg[vgAt];
            if (mapAt < reverseCount) blend[v * stride + i] = static_cast<uint8_t>(reverse[mapAt]);
        }
    }
}

}  // namespace efmivulkan
