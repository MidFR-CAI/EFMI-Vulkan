// The descriptor bindings a SPIR-V shader module declares: which (set, binding) it reads as a
// uniform buffer, a storage buffer, or something else (images, samplers). Compilers drop
// what a shader does not use, so declared is as good as used.
//
// EFMIVulkan needs it to find where a draw's vertex shader really reads its bones and its
// instance config: guessing from what the sets hold picked the wrong buffer for some pipelines.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace efmivulkan::spirv {

enum class Kind { kUniformBuffer, kStorageBuffer, kOther };

struct Resource {
    uint32_t set = 0, binding = 0;
    Kind kind = Kind::kOther;
    bool operator==(const Resource& o) const { return set == o.set && binding == o.binding && kind == o.kind; }
};

// Empty when the code is not SPIR-V.
std::vector<Resource> Resources(const uint32_t* code, size_t words);

}  // namespace efmivulkan::spirv
