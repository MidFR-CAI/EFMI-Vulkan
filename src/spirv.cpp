#include "spirv.h"

#include <unordered_map>
#include <unordered_set>

namespace efmivulkan::spirv {
namespace {

constexpr uint32_t kMagic = 0x07230203;
// Opcodes.
constexpr uint32_t kOpTypeArray = 28, kOpTypeRuntimeArray = 29, kOpTypeStruct = 30, kOpTypePointer = 32, kOpVariable = 59, kOpDecorate = 71;
// Decorations.
constexpr uint32_t kBlock = 2, kBufferBlock = 3, kBinding = 33, kDescriptorSet = 34;
// Storage classes.
constexpr uint32_t kUniform = 2, kStorageBuffer = 12;

}  // namespace

std::vector<Resource> Resources(const uint32_t* code, size_t words) {
    std::vector<Resource> out;
    if (!code || words < 5 || code[0] != kMagic) return out;
    std::unordered_map<uint32_t, uint32_t> set, binding;    // Id -> decoration value.
    std::unordered_set<uint32_t> block, bufferBlock;        // Struct type ids.
    std::unordered_map<uint32_t, uint32_t> pointee, array;  // Pointer type -> type; array type -> element.
    struct Variable {
        uint32_t id, type, storage;
    };
    std::vector<Variable> variables;
    for (size_t at = 5; at < words;) {
        const uint32_t count = code[at] >> 16, op = code[at] & 0xffff;
        if (count == 0 || at + count > words) break;
        const uint32_t* w = code + at;
        if (op == kOpDecorate && count >= 3) {
            if (w[2] == kDescriptorSet && count >= 4) set[w[1]] = w[3];
            if (w[2] == kBinding && count >= 4) binding[w[1]] = w[3];
            if (w[2] == kBlock) block.insert(w[1]);
            if (w[2] == kBufferBlock) bufferBlock.insert(w[1]);
        } else if (op == kOpTypePointer && count >= 4) {
            pointee[w[1]] = w[3];
        } else if ((op == kOpTypeArray || op == kOpTypeRuntimeArray) && count >= 3) {
            array[w[1]] = w[2];
        } else if (op == kOpVariable && count >= 4) {
            variables.push_back({w[2], w[1], w[3]});
        }
        at += count;
    }
    for (const Variable& v : variables) {
        const auto s = set.find(v.id), b = binding.find(v.id);
        if (s == set.end() || b == binding.end()) continue;
        uint32_t type = pointee.count(v.type) ? pointee[v.type] : 0;
        for (int depth = 0; depth < 4 && array.count(type); ++depth) type = array[type];  // Descriptor arrays.
        Resource r{s->second, b->second, Kind::kOther};
        if (v.storage == kStorageBuffer || (v.storage == kUniform && bufferBlock.count(type))) {
            r.kind = Kind::kStorageBuffer;
        } else if (v.storage == kUniform) {
            r.kind = Kind::kUniformBuffer;
        }
        out.push_back(r);
    }
    return out;
}

}  // namespace efmivulkan::spirv
