// Hashes that must stay bit-exact with 3DMigoto (XXMI-Libs):
//   crc32c_hw(seed, ...)  crc32c-hw-1.0.5, chainable, seed 0
//   buffer hash           crc(crc(0, data, ByteWidth), &D3D11_BUFFER_DESC, 24)  [HackerDevice::CreateBuffer]
//                         With no initial data the first step is skipped, so the seed is 0.
//   shader hash           FNV-1 64 with an initial value of 0                   [util.h fnv_64_buf]
#pragma once
#include <cstddef>
#include <cstdint>

namespace efmivulkan {

// Layout of D3D11_BUFFER_DESC, kept here so the DLL does not need d3d11.h.
struct BufferDesc11 {
    uint32_t byteWidth;
    uint32_t usage;           // D3D11_USAGE: 0 default, 1 immutable, 2 dynamic, 3 staging
    uint32_t bindFlags;       // VB 1, IB 2, CB 4, SRV 8, SO 0x10, UAV 0x80
    uint32_t cpuAccessFlags;  // WRITE 0x10000, READ 0x20000
    uint32_t miscFlags;       // DRAWINDIRECT 0x10, RAW 0x20, STRUCTURED 0x40
    uint32_t structureByteStride;
};
static_assert(sizeof(BufferDesc11) == 24);

// Layout of D3D11_TEXTURE2D_DESC.
struct TextureDesc11 {
    uint32_t width, height, mipLevels, arraySize, format, sampleCount, sampleQuality;
    uint32_t usage, bindFlags, cpuAccessFlags, miscFlags;
};
static_assert(sizeof(TextureDesc11) == 44);

uint32_t Crc32c(uint32_t seed, const void* data, size_t length);

// Same result as Crc32c, for write-combined memory (upload heaps). Plain reads
// from WC memory are extremely slow; this uses SSE4.1 streaming loads.
uint32_t Crc32cWriteCombined(uint32_t seed, const void* data, size_t length);

// Copies out of write-combined memory, for reading what the game put in upload heaps.
void CopyWriteCombined(void* to, const void* from, size_t length);

inline uint32_t BufferHash(uint32_t dataCrc, const BufferDesc11& desc) {
    return Crc32c(dataCrc, &desc, sizeof(desc));
}

uint64_t Fnv64(const void* data, size_t length);

}  // namespace efmivulkan
