// Index buffer region hashes, as the XXMI fork of 3DMigoto computes them with
// track_region_hashes: crc32c, seed 0, over exactly the bytes a draw reads,
//
//   offset = binding offset + index size * firstIndex
//   size   = index size * indexCount         (index size 4 for UINT32, 2 for UINT16)
//
// with nothing of the buffer description mixed in. EFMI keys every component on
// one of these (TextureOverride hash + match_index_count).
//
// The hash needs the index data on the CPU at draw time. Uploads are seen as they are
// submitted, so a copy of each one small enough to be an index buffer is kept, up to a
// memory cap, oldest dropped first.
//
// Addresses: Vulkan buffers have none by default, so each buffer gets an id and a byte
// in it is (id << 36) + offset. See BufferKey.
#pragma once
#include <cstddef>
#include <cstdint>

namespace efmivulkan::ibregion {

inline uint64_t BufferKey(uint32_t bufferId, uint64_t offset) { return (uint64_t(bufferId) << 36) + offset; }

// data: the upload's source, read now.
void OnUpload(uint64_t key, const uint8_t* data, uint64_t size);
void OnOverwritten(uint64_t key, uint64_t size);

// The region hash of bytes [key, key + size), or 0 when they were never seen uploaded.
// Cached, so a draw seen every frame hashes once.
uint32_t RegionHash(uint64_t key, uint64_t size);
// For index data the CPU can read in place (host-visible memory): hashes it and caches.
uint32_t RegionHashOf(uint64_t key, const uint8_t* data, uint64_t size);

struct Stats {
    size_t keptBuffers, keptBytes, cachedHashes, misses;
};
Stats GetStats();

}  // namespace efmivulkan::ibregion
