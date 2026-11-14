// The SPIR-V reader: which buffers a shader module declares, at which set and binding.
#include <cstdio>
#include <vector>

#include "spirv.h"

using namespace efmivulkan::spirv;

static int g_failed = 0;
static void Check(bool ok, const char* what) {
    printf("%-64s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failed;
}

static uint32_t Op(uint32_t count, uint32_t op) { return count << 16 | op; }

int main() {
    // A uniform buffer (Block, Uniform) at set 1 binding 3, a storage buffer (StorageBuffer) at
    // set 2 binding 5, an older storage buffer (BufferBlock, Uniform) at set 0 binding 1, and a
    // variable with no binding (an input).
    const std::vector<uint32_t> code = {
        0x07230203, 0x00010000, 0, 20, 0,
        Op(3, 71), 2, 2,          // OpDecorate %2 Block
        Op(3, 71), 7, 3,          // OpDecorate %7 BufferBlock
        Op(4, 71), 4, 34, 1,      // %4 DescriptorSet 1
        Op(4, 71), 4, 33, 3,      // %4 Binding 3
        Op(4, 71), 6, 34, 2,      // %6 DescriptorSet 2
        Op(4, 71), 6, 33, 5,      // %6 Binding 5
        Op(4, 71), 9, 34, 0,      // %9 DescriptorSet 0
        Op(4, 71), 9, 33, 1,      // %9 Binding 1
        Op(3, 22), 1, 32,         // %1 = OpTypeFloat 32
        Op(3, 30), 2, 1,          // %2 = OpTypeStruct %1
        Op(4, 32), 3, 2, 2,       // %3 = OpTypePointer Uniform %2
        Op(4, 59), 3, 4, 2,       // %4 = OpVariable %3 Uniform
        Op(4, 32), 5, 12, 2,      // %5 = OpTypePointer StorageBuffer %2
        Op(4, 59), 5, 6, 12,      // %6 = OpVariable %5 StorageBuffer
        Op(3, 30), 7, 1,          // %7 = OpTypeStruct %1
        Op(4, 32), 8, 2, 7,       // %8 = OpTypePointer Uniform %7
        Op(4, 59), 8, 9, 2,       // %9 = OpVariable %8 Uniform
        Op(4, 32), 10, 1, 1,      // %10 = OpTypePointer Input %1
        Op(4, 59), 10, 11, 1,     // %11 = OpVariable %10 Input
    };
    const std::vector<Resource> r = Resources(code.data(), code.size());
    Check(r.size() == 3, "three resources, the input left out");
    const auto has = [&](Resource want) {
        for (const Resource& x : r) {
            if (x == want) return true;
        }
        return false;
    };
    Check(has({1, 3, Kind::kUniformBuffer}), "uniform buffer at set 1 binding 3");
    Check(has({2, 5, Kind::kStorageBuffer}), "storage buffer at set 2 binding 5");
    Check(has({0, 1, Kind::kStorageBuffer}), "BufferBlock storage buffer at set 0 binding 1");
    Check(Resources(nullptr, 0).empty() && Resources(code.data(), 3).empty(), "not SPIR-V: nothing");
    printf(g_failed ? "%d FAILED\n" : "all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
