#include "hash.h"

#include <immintrin.h>

#include <algorithm>
#include <cstring>

namespace efmivulkan {

static uint64_t Crc32cRaw(uint64_t state, const uint8_t* p, size_t n) {
    while (n && (reinterpret_cast<uintptr_t>(p) & 7)) {
        state = _mm_crc32_u8(static_cast<uint32_t>(state), *p++);
        --n;
    }
    while (n >= 8) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        state = _mm_crc32_u64(state, v);
        p += 8;
        n -= 8;
    }
    while (n--) state = _mm_crc32_u8(static_cast<uint32_t>(state), *p++);
    return state;
}

uint32_t Crc32c(uint32_t seed, const void* data, size_t length) {
    uint64_t s = seed ^ 0xffffffffu;
    s = Crc32cRaw(s, static_cast<const uint8_t*>(data), length);
    return static_cast<uint32_t>(s) ^ 0xffffffffu;
}

uint32_t Crc32cWriteCombined(uint32_t seed, const void* data, size_t length) {
    alignas(64) uint8_t chunk[16 * 1024];
    uint64_t s = seed ^ 0xffffffffu;
    auto cur = static_cast<const uint8_t*>(data);
    const uint8_t* const end = cur + length;
    _mm_mfence();
    while (cur < end) {
        // Streaming loads must be 16-byte aligned. Rounding out to 16 bytes
        // cannot cross into an unmapped page.
        auto base = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(cur) & ~uintptr_t(15));
        const size_t head = static_cast<size_t>(cur - base);
        const size_t want = (static_cast<size_t>(end - base) + 15) & ~size_t(15);
        const size_t block = std::min(sizeof(chunk), want);
        for (size_t i = 0; i < block; i += 16) {
            __m128i v = _mm_stream_load_si128(reinterpret_cast<const __m128i*>(base + i));
            _mm_store_si128(reinterpret_cast<__m128i*>(chunk + i), v);
        }
        const size_t use = std::min(block - head, static_cast<size_t>(end - cur));
        s = Crc32cRaw(s, chunk + head, use);
        cur += use;
    }
    return static_cast<uint32_t>(s) ^ 0xffffffffu;
}

void CopyWriteCombined(void* to, const void* from, size_t length) {
    alignas(16) uint8_t block[16];
    auto out = static_cast<uint8_t*>(to);
    auto cur = static_cast<const uint8_t*>(from);
    const uint8_t* const end = cur + length;
    _mm_mfence();
    while (cur < end) {
        auto base = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(cur) & ~uintptr_t(15));
        _mm_store_si128(reinterpret_cast<__m128i*>(block), _mm_stream_load_si128(reinterpret_cast<const __m128i*>(base)));
        const size_t head = static_cast<size_t>(cur - base);
        const size_t use = std::min(16 - head, static_cast<size_t>(end - cur));
        std::memcpy(out, block + head, use);
        out += use;
        cur += use;
    }
}

uint64_t Fnv64(const void* data, size_t length) {
    constexpr uint64_t kPrime = 0x100000001b3ULL;
    uint64_t h = 0;  // 3DMigoto starts from 0, not the usual FNV offset basis.
    auto p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; ++i) {
        h *= kPrime;
        h ^= p[i];
    }
    return h;
}

}  // namespace efmivulkan
