// 3DMigoto's texture hash (XXMI fork, texture_hash = 0, the legacy data hash), worked out
// from what a Vulkan upload gives:
//
//   data = crc32c(0, mip 0 bytes, min(width * height, mip 0 byte size))
//   hash = crc32c(data, D3D11_TEXTURE2D_DESC as the DX11 game created it, 44 bytes)
//
// Vulkan gives the pixels, size and mip count, but not what Unity's DX11 renderer puts in
// the description's format (typed, typeless or sRGB) and usage (default or immutable). So
// every plausible description is tried, and which one matched a mod's hash is reported.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace efmivulkan::texhash {

// Bytes of one mip level, tightly packed: block-compressed sizes round up to 4x4 blocks.
// 0 for a format this does not know.
uint64_t MipBytes(uint32_t vkFormat, uint32_t width, uint32_t height);

// The data half: over at most width * height bytes of mip 0.
uint32_t DataHash(const uint8_t* mip0, uint64_t mip0Bytes, uint32_t width, uint32_t height, bool writeCombined);

struct Candidate {
    uint32_t hash;
    uint32_t dxgiFormat;
    uint32_t usage;  // D3D11_USAGE: 0 default, 1 immutable.
};

// The full hash with the description Endfield's DX11 renderer uses: the typed format of
// the Vulkan format's sRGB-ness, default usage. Hash 0 for a format this does not know.
Candidate Hash(uint32_t dataHash, uint32_t vkFormat, uint32_t width, uint32_t height, uint32_t mips);

// The full hashes for every description a DX11 game might have used. Empty for a
// format with no DXGI equivalent this knows.
std::vector<Candidate> Candidates(uint32_t dataHash, uint32_t vkFormat, uint32_t width, uint32_t height, uint32_t mips);

std::string Describe(const Candidate& c);

}  // namespace efmivulkan::texhash
