#include "shapekeys.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace efmivulkan::shapekeys {

bool LooksLikeControl(const uint32_t* control) {
    for (int k = 1; k < kKeysPerBatch; ++k) {
        if (control[k] < control[k - 1]) return false;
    }
    const float* weights = Weights(control);
    for (int k = 0; k < kKeysPerBatch; ++k) {
        if (!std::isfinite(weights[k]) || std::fabs(weights[k]) > 1000.0f) return false;
    }
    return true;
}

float HalfToFloat(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000) << 16;
    uint32_t exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff, bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {  // Subnormal: normalise it.
            exponent = 127 - 15 + 1;
            while (!(mantissa & 0x400)) {
                mantissa <<= 1;
                --exponent;
            }
            bits = sign | (exponent << 23) | ((mantissa & 0x3ff) << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000 | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

void Apply(const uint32_t* offsets, size_t offsetCount, const uint32_t* ids, const uint16_t* deltas, size_t entries,
           const Batch* batches, size_t batchCount, uint32_t vertices, std::vector<float>* out) {
    out->assign(size_t(vertices) * kFloatsPerVertex, 0.0f);
    std::vector<float> weightSum(vertices, 0.0f);
    for (size_t b = 0; b < batchCount; ++b) {
        if (!batches[b].weights || (b + 1) * kKeysPerBatch > offsetCount) continue;
        const uint32_t* keyOffsets = offsets + b * kKeysPerBatch;
        // The last offset only ends key 126, as in the game's shader.
        for (int k = 0; k + 1 < kKeysPerBatch; ++k) {
            const float weight = batches[b].weights[k];
            if (weight == 0.0f) continue;
            const size_t begin = size_t(batches[b].customOffset) + keyOffsets[k];
            const size_t end = std::min(size_t(batches[b].customOffset) + keyOffsets[k + 1], entries);
            for (size_t e = begin; e < end; ++e) {
                const uint32_t v = ids[e];
                if (v >= vertices) continue;
                float* to = out->data() + size_t(v) * kFloatsPerVertex;
                const uint16_t* from = deltas + e * kFloatsPerVertex;
                for (int c = 0; c < kFloatsPerVertex; ++c) to[c] += HalfToFloat(from[c]) * weight;
                weightSum[v] += weight;
            }
        }
    }
    for (uint32_t v = 0; v < vertices; ++v) {
        if (weightSum[v] <= 1.0f) continue;
        float* normal = out->data() + size_t(v) * kFloatsPerVertex + 3;
        for (int c = 0; c < 3; ++c) normal[c] /= weightSum[v];
    }
}

}  // namespace efmivulkan::shapekeys
