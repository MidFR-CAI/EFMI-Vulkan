// Which game textures the mods replace. EFMI names a mod's texture after the hash of the
// game texture it stands for ("Components-0-9 t=3156b9c5 BC7-sRGB.dds"), and its
// TextureOverride sections use the same hashes. Those are 3DMigoto's DX11 texture hashes
// (texhash.h), worked out here from each image's upload.
//
// Milestone 2c, first step: report which uploads match and which description variant
// made the match, so the next step can rely on one.
#pragma once
#include <cstdint>
#include <set>

namespace efmivulkan::textures {

// The hashes mods name. Job thread, once the mods are read.
void SetTargets(std::set<uint32_t> hashes);

// One upload of an image's mip 0, read at submit. imageId: the layer's id for the image.
void OnImageUpload(uint64_t imageId, uint32_t vkFormat, uint32_t width, uint32_t height, uint32_t mips, const uint8_t* mip0, uint64_t bytes);
void OnImageDestroyed(uint64_t imageId);

// Streamed copies: Unity on Vulkan binds a smaller image of a texture while its full size
// streams in, or far away. Its mip 0 is the full texture's mip N, byte for byte, so it hashes
// to nothing a mod names. A lower mip (N > 0) of an image that matched: its data hash is kept,
// and an image whose mip 0 has that data is the same texture, smaller, under the same hash;
// whichever of the two goes up first.
void OnMipUpload(uint64_t imageId, uint32_t vkFormat, uint32_t mip, uint32_t width, uint32_t height, const uint8_t* data, uint64_t bytes);

// The mod hash an image matched, or 0.
uint32_t HashOf(uint64_t imageId);

void LogStats();

}  // namespace efmivulkan::textures
