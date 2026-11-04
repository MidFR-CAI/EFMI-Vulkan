// Checks the hash code against values that came from real mods, not from itself.
#include <cstdio>
#include <cstring>
#include <vector>

#include <fstream>
#include <sstream>
#include <string>

#include "hash.h"
#include "shapekeys.h"
#include "skeleton.h"

using namespace efmivulkan;

static int g_failed = 0;

static void Expect(const char* name, uint32_t got, uint32_t want) {
    const bool ok = got == want;
    printf("%-52s %08x %s\n", name, got, ok ? "ok" : "FAIL");
    if (!ok) {
        printf("  expected %08x\n", want);
        ++g_failed;
    }
}

int main() {
    // Standard CRC-32C check value.
    Expect("crc32c('123456789')", Crc32c(0, "123456789", 9), 0xe3069283);

    // Appending must equal hashing in one go: 3DMigoto chains data then desc.
    Expect("crc32c chained", Crc32c(Crc32c(0, "1234", 4), "56789", 5), 0xe3069283);

    // Hashes from installed mods with no initial data, hash = crc(0, desc).
    // Aemeath: shape key buffers, 78124 vertices at the strides the ini states.
    constexpr uint32_t kVbSrvUav = 0x89;
    Expect("Aemeath ShapeKeyScale 61ccff68", BufferHash(0, BufferDesc11{78124 * 4, 0, kVbSrvUav, 0, 0, 0}), 0x61ccff68);
    Expect("Aemeath ShapeKeyOffsets 15eeff11", BufferHash(0, BufferDesc11{78124 * 24, 0, kVbSrvUav, 0, 0, 0}), 0x15eeff11);
    // Shared skeleton constant buffer: dynamic, CPU write, 16 KB.
    Expect("bone CB f02baf77", BufferHash(0, BufferDesc11{16384, 2, 4, 0x10000, 0, 0}), 0xf02baf77);

    // The write-combined path must agree at every alignment and around chunk edges.
    std::vector<uint8_t> data(64 * 1024 + 123);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 2654435761u >> 24);
    for (size_t offset : {0, 1, 7, 8, 15, 16, 17, 63}) {
        for (size_t length : {0, 1, 15, 16, 17, 255, 4096, 16383, 16384, 16385, 40000, 64000}) {
            if (Crc32cWriteCombined(0, data.data() + offset, length) != Crc32c(0, data.data() + offset, length)) {
                printf("write-combined mismatch offset=%zu length=%zu FAIL\n", offset, length);
                ++g_failed;
            }
        }
    }

    // Bone data as logged from the game, and buffers that are not bones.
    {
        const Row real[6] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0.977f, 0.056f, -0.207f, 21.969f},
                             {0.087f, 0.778f, 0.622f, -55.867f}, {0.196f, -0.625f, 0.755f, 11.948f}};
        const Row flags[3] = {{1, 0, 0, 1}, {1, 0, 0, 0}, {0, 0, 0, 0}};
        const Row params[3] = {{1, 1, 0, 1}, {0.5f, 1, 1, 5}, {0, 1, 0, 0}};
        const Row empty[6] = {};
        Expect("bones: real data, first bone unused", LooksLikeBones(real, 6), 1);
        Expect("bones: flag block rejected", LooksLikeBones(flags, 3), 0);
        Expect("bones: parameter block rejected", LooksLikeBones(params, 3), 0);
        Expect("bones: empty buffer rejected", LooksLikeBones(empty, 6), 0);
    }

    // SkeletonMerger: bone i lands at vg_offset + i, and an empty bone keeps the old one.
    {
        std::vector<float> merged(kMaxMergedBones * kRowsPerBone * 4, 7.0f);
        Row game[6] = {};
        game[0][3] = 5;
        MergeBones(game, reinterpret_cast<Row*>(merged.data()), 10, 2);
        Expect("merge: bone copied to its offset", merged[10 * 12 + 3] == 5 && merged[10 * 12] == 0, 1);
        Expect("merge: empty bone left alone", merged[11 * 12] == 7, 1);
        Expect("merge: neighbours untouched", merged[9 * 12] == 7 && merged[12 * 12] == 7, 1);
    }

    // SkeletonRemapper: output bone i is merged[forward[remap * 512 + i]].
    {
        std::vector<float> merged(kMaxMergedBones * kRowsPerBone * 4), out(merged.size());
        for (int b = 0; b < kMaxMergedBones; ++b) merged[b * 12] = static_cast<float>(b);
        std::vector<uint16_t> forward(kRemapBoneCount * 2);
        forward[kRemapBoneCount + 0] = 300;
        forward[kRemapBoneCount + 1] = 400;
        RemapBones(reinterpret_cast<const Row*>(merged.data()), forward.data(), forward.size(), 1, 2, reinterpret_cast<Row*>(out.data()));
        Expect("remap: uses the second map", out[0] == 300 && out[12] == 400, 1);
        Expect("remap: bones past vg_count stay merged", out[2 * 12] == 2, 1);
    }

    // BlendRemapper: ids come from the 16-bit list through the reverse map, weights stay.
    {
        uint8_t blend[16] = {1, 2, 3, 4, 5, 6, 7, 8, 100, 101, 102, 103, 104, 105, 106, 107};
        std::vector<uint16_t> vertexVg = {300, 301, 0, 0, 0, 0, 0, 0};
        std::vector<uint16_t> reverse(kRemapBoneCount * 2);
        reverse[kRemapBoneCount + 300] = 9;
        reverse[kRemapBoneCount + 301] = 10;
        RemapBlend(blend, sizeof(blend), 16, vertexVg.data(), vertexVg.size(), reverse.data(), reverse.size(), 1);
        Expect("blend remap: 16-bit ids mapped to 8-bit", blend[0] == 9 && blend[1] == 10 && blend[2] == 0, 1);
        Expect("blend remap: weights untouched", blend[8] == 100 && blend[15] == 107, 1);
    }

    {
        uint8_t from[64], to[64] = {};
        for (int i = 0; i < 64; ++i) from[i] = static_cast<uint8_t>(i + 1);
        CopyWriteCombined(to + 3, from + 5, 40);
        Expect("write-combined copy", std::memcmp(to + 3, from + 5, 40) == 0 && to[2] == 0 && to[43] == 0, 1);
    }

    {
        using namespace efmivulkan::shapekeys;
        Expect("half 1.0", HalfToFloat(0x3c00) == 1.0f, 1);
        Expect("half -2.0", HalfToFloat(0xc000) == -2.0f, 1);
        Expect("half 0.5", HalfToFloat(0x3800) == 0.5f, 1);

        // Two batches. Batch 0: key 0 has entries 0-1, key 1 has entry 2. Batch 1
        // starts at entry 3: its key 0 has entry 3.
        std::vector<uint32_t> offsets(2 * kKeysPerBatch, 0);
        for (int k = 1; k < kKeysPerBatch; ++k) offsets[k] = k == 1 ? 2 : 3;
        for (int k = 1; k < kKeysPerBatch; ++k) offsets[kKeysPerBatch + k] = 1;
        const uint32_t ids[4] = {0, 1, 1, 2};
        std::vector<uint16_t> deltas(4 * kFloatsPerVertex, 0x3c00);  // All 1.0.
        float w0[kKeysPerBatch] = {0.5f, 2.0f}, w1[kKeysPerBatch] = {1.0f};
        const Batch batches[2] = {{0, w0}, {3, w1}};
        std::vector<float> out;
        Apply(offsets.data(), offsets.size(), ids, deltas.data(), 4, batches, 2, 3, &out);
        Expect("shape keys: vertex 0 moved by key 0", out[0] == 0.5f && out[3] == 0.5f, 1);
        Expect("shape keys: vertex 1 sums keys 0 and 1", out[6] == 2.5f, 1);
        Expect("shape keys: vertex 1 normal divided by weight 2.5", out[9] == 1.0f, 1);
        Expect("shape keys: batch 1 reads from its own offset", out[12] == 1.0f && out[15] == 1.0f, 1);
        const Batch unseen[2] = {{0, w0}, {3, nullptr}};
        Apply(offsets.data(), offsets.size(), ids, deltas.data(), 4, unseen, 2, 3, &out);
        Expect("shape keys: a batch not run yet adds nothing", out[12] == 0.0f, 1);

        uint32_t control[kControlRows * 4] = {};
        for (int k = 0; k < kKeysPerBatch; ++k) control[k] = k * 10;
        Expect("control: rising offsets look right", LooksLikeControl(control), 1);
        control[5] = 1;
        Expect("control: falling offsets do not", LooksLikeControl(control), 0);
    }

    // FNV-1 with a zero start, as 3DMigoto's fnv_64_buf: 0*p^'a' = 0x61.
    if (Fnv64("a", 1) != 0x61 || Fnv64("ab", 2) != ((0x61ull * 0x100000001b3ULL) ^ 0x62)) {
        printf("fnv FAIL\n");
        ++g_failed;
    }

    printf(g_failed ? "%d FAILED\n" : "all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
