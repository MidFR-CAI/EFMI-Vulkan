#include "texhash.h"

#include <algorithm>
#include <cstdio>

#include "hash.h"

namespace efmivulkan::texhash {
namespace {

struct FormatInfo {
    uint32_t vk;
    uint32_t blockBytes;  // Bytes per 4x4 block, or 0 for uncompressed.
    uint32_t pixelBytes;  // Bytes per pixel when uncompressed.
    uint32_t dxgi[3];     // The DXGI family: typeless, linear, sRGB (0 where none).
};

// VkFormat values from vulkan_core.h, DXGI values from dxgiformat.h.
constexpr FormatInfo kFormats[] = {
    {9, 0, 1, {60, 61, 0}},       // R8_UNORM
    {16, 0, 2, {48, 49, 0}},      // R8G8_UNORM
    {37, 0, 4, {27, 28, 29}},     // R8G8B8A8_UNORM
    {43, 0, 4, {27, 28, 29}},     // R8G8B8A8_SRGB
    {44, 0, 4, {90, 87, 91}},     // B8G8R8A8_UNORM
    {50, 0, 4, {90, 87, 91}},     // B8G8R8A8_SRGB
    {97, 0, 8, {9, 10, 0}},       // R16G16B16A16_SFLOAT
    {109, 0, 16, {1, 2, 0}},      // R32G32B32A32_SFLOAT
    {131, 8, 0, {70, 71, 72}},    // BC1_RGB_UNORM
    {132, 8, 0, {70, 71, 72}},    // BC1_RGB_SRGB
    {133, 8, 0, {70, 71, 72}},    // BC1_RGBA_UNORM
    {134, 8, 0, {70, 71, 72}},    // BC1_RGBA_SRGB
    {135, 16, 0, {73, 74, 75}},   // BC2_UNORM
    {136, 16, 0, {73, 74, 75}},   // BC2_SRGB
    {137, 16, 0, {76, 77, 78}},   // BC3_UNORM
    {138, 16, 0, {76, 77, 78}},   // BC3_SRGB
    {139, 8, 0, {79, 80, 0}},     // BC4_UNORM
    {140, 8, 0, {79, 81, 0}},     // BC4_SNORM
    {141, 16, 0, {82, 83, 0}},    // BC5_UNORM
    {142, 16, 0, {82, 84, 0}},    // BC5_SNORM
    {143, 16, 0, {94, 95, 0}},    // BC6H_UFLOAT
    {144, 16, 0, {94, 96, 0}},    // BC6H_SFLOAT
    {145, 16, 0, {97, 98, 99}},   // BC7_UNORM
    {146, 16, 0, {97, 98, 99}},   // BC7_SRGB
};

const FormatInfo* Find(uint32_t vkFormat) {
    for (const FormatInfo& f : kFormats) {
        if (f.vk == vkFormat) return &f;
    }
    return nullptr;
}

}  // namespace

uint64_t MipBytes(uint32_t vkFormat, uint32_t width, uint32_t height) {
    const FormatInfo* f = Find(vkFormat);
    if (!f) return 0;
    if (f->blockBytes) return uint64_t((width + 3) / 4) * ((height + 3) / 4) * f->blockBytes;
    return uint64_t(width) * height * f->pixelBytes;
}

uint32_t DataHash(const uint8_t* mip0, uint64_t mip0Bytes, uint32_t width, uint32_t height, bool writeCombined) {
    // Legacy 3DMigoto: "length_v12" is width * height * array size, in texels not bytes,
    // and wins when it is the shorter.
    const size_t length = static_cast<size_t>(std::min<uint64_t>(uint64_t(width) * height, mip0Bytes));
    return writeCombined ? Crc32cWriteCombined(0, mip0, length) : Crc32c(0, mip0, length);
}

Candidate Hash(uint32_t dataHash, uint32_t vkFormat, uint32_t width, uint32_t height, uint32_t mips) {
    const FormatInfo* f = Find(vkFormat);
    if (!f) return {0, 0, 0};
    // Measured on Endfield (23 mod textures): the DX11 game uses the typed format with the
    // Vulkan format's own sRGB-ness, and default usage.
    const bool srgb = vkFormat == 43 || vkFormat == 50 || vkFormat == 132 || vkFormat == 134 || vkFormat == 136 || vkFormat == 138 || vkFormat == 146;
    const uint32_t dxgi = srgb && f->dxgi[2] ? f->dxgi[2] : f->dxgi[1];
    const TextureDesc11 desc{width, height, mips, 1, dxgi, 1, 0, 0, 0x8, 0, 0};
    return {Crc32c(dataHash, &desc, sizeof(desc)), dxgi, 0};
}

std::vector<Candidate> Candidates(uint32_t dataHash, uint32_t vkFormat, uint32_t width, uint32_t height, uint32_t mips) {
    std::vector<Candidate> out;
    const FormatInfo* f = Find(vkFormat);
    if (!f) return out;
    for (uint32_t dxgi : f->dxgi) {
        if (!dxgi) continue;
        for (uint32_t usage : {0u, 1u}) {
            // Width, Height, MipLevels, ArraySize, Format, SampleDesc, Usage, BindFlags
            // (shader resource), CPUAccessFlags, MiscFlags.
            const TextureDesc11 desc{width, height, mips, 1, dxgi, 1, 0, usage, 0x8, 0, 0};
            out.push_back({Crc32c(dataHash, &desc, sizeof(desc)), dxgi, usage});
        }
    }
    return out;
}

std::string Describe(const Candidate& c) {
    char text[64];
    snprintf(text, sizeof(text), "DXGI format %u, usage %s", c.dxgiFormat, c.usage ? "immutable" : "default");
    return text;
}

}  // namespace efmivulkan::texhash
