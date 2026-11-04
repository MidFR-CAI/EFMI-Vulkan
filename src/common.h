#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>

#include "log.h"

namespace efmivulkan {

struct Stats {
    std::atomic<uint64_t> presents{0}, ecls{0}, maps{0};
    std::atomic<uint64_t> buffersCreated{0}, psosCreated{0};
    std::atomic<uint64_t> copies{0}, copiesHashed{0}, copiesSkippedDynamic{0}, copiesOtherHeap{0};
    std::atomic<uint64_t> bytesHashed{0}, faults{0};
    std::atomic<uint64_t> devices{0};
};
inline Stats g_stats;

// After this many faults the mods stop drawing: a fault part way through a draw can
// leave the game's bindings changed, so carrying on risks a broken or crashing frame.
// F9 tries again.
constexpr uint64_t kFailSafeFaults = 16;
inline std::atomic<bool> g_failSafe{false};
inline std::atomic<uint64_t> g_faultsSinceReset{0};

inline int GuardFilter(const char* where, unsigned long code) {
    if (g_stats.faults.fetch_add(1, std::memory_order_relaxed) < 32)
        LogError("fault 0x%08lx in %s (hook kept running)", code, where);
    if (g_faultsSinceReset.fetch_add(1, std::memory_order_relaxed) + 1 == kFailSafeFaults) g_failSafe.store(true);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Runs our own hook logic so a fault in it can never take the game down.
// The game's call to the original function always stays outside of this.
template <class F>
inline void Guard(const char* where, F&& fn) noexcept {
    __try {
        fn();
    } __except (GuardFilter(where, GetExceptionCode())) {
    }
}

}  // namespace efmivulkan
