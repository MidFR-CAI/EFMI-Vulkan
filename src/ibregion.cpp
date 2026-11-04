#include "ibregion.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "hash.h"

namespace efmivulkan::ibregion {
namespace {

constexpr uint64_t kMaxKeptUpload = 16ull << 20;  // Bigger is not an index buffer of one mesh.
constexpr uint64_t kMaxKeptBytes = 512ull << 20;

struct Kept {
    uint64_t size;
    uint64_t order;  // Lower is older.
    std::shared_ptr<const std::vector<uint8_t>> data;
};

std::shared_mutex g_mu;
std::map<uint64_t, Kept>* g_kept = new std::map<uint64_t, Kept>();  // By start address.
std::map<std::pair<uint64_t, uint64_t>, uint32_t>* g_hashes = new std::map<std::pair<uint64_t, uint64_t>, uint32_t>();
uint64_t g_keptBytes = 0, g_order = 0, g_misses = 0;
uint64_t g_maxRegion = 0;  // Longest cached region: where a scan for overlaps must start.

// Drops kept uploads and cached hashes overlapping [va, va + size). Caller holds g_mu.
void EraseOverlaps(uint64_t va, uint64_t size) {
    auto it = g_kept->lower_bound(va);
    if (it != g_kept->begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second.size > va) it = prev;
    }
    while (it != g_kept->end() && it->first < va + size) {
        g_keptBytes -= it->second.size;
        it = g_kept->erase(it);
    }
    // Hashes are keyed by start: a region overlapping [va, va + size) starts no earlier
    // than va - g_maxRegion.
    const uint64_t from = va > g_maxRegion ? va - g_maxRegion : 0;
    for (auto h = g_hashes->lower_bound({from, 0}); h != g_hashes->end() && h->first.first < va + size;) {
        if (h->first.first + h->first.second > va) {
            h = g_hashes->erase(h);
        } else {
            ++h;
        }
    }
}

}  // namespace

void OnUpload(uint64_t va, const uint8_t* data, uint64_t size) {
    if (size > kMaxKeptUpload || size % 2) {
        OnOverwritten(va, size);
        return;
    }
    auto copy = std::make_shared<std::vector<uint8_t>>(data, data + size);
    std::unique_lock lock(g_mu);
    EraseOverlaps(va, size);
    while (g_keptBytes + size > kMaxKeptBytes && !g_kept->empty()) {
        auto oldest = g_kept->begin();
        for (auto it = g_kept->begin(); it != g_kept->end(); ++it) {
            if (it->second.order < oldest->second.order) oldest = it;
        }
        g_keptBytes -= oldest->second.size;
        g_kept->erase(oldest);
    }
    (*g_kept)[va] = Kept{size, g_order++, std::move(copy)};
    g_keptBytes += size;
}

void OnOverwritten(uint64_t va, uint64_t size) {
    std::unique_lock lock(g_mu);
    EraseOverlaps(va, size);
}

uint32_t RegionHash(uint64_t va, uint64_t size) {
    if (!size) return 0;
    std::shared_ptr<const std::vector<uint8_t>> data;
    uint64_t at = 0;
    {
        std::shared_lock lock(g_mu);
        const auto cached = g_hashes->find({va, size});
        if (cached != g_hashes->end()) return cached->second;
        auto it = g_kept->upper_bound(va);
        if (it != g_kept->begin()) {
            --it;
            if (va + size <= it->first + it->second.size) {
                data = it->second.data;
                at = va - it->first;
            }
        }
    }
    // A miss is cached as 0 too, so a draw whose data was never seen costs one lookup a
    // frame; an upload to the range clears it.
    const uint32_t hash = data ? Crc32c(0, data->data() + at, static_cast<size_t>(size)) : 0;
    std::unique_lock lock(g_mu);
    if (!data) ++g_misses;
    (*g_hashes)[{va, size}] = hash;
    g_maxRegion = std::max(g_maxRegion, size);
    return hash;
}

uint32_t RegionHashOf(uint64_t va, const uint8_t* data, uint64_t size) {
    {
        // Index data in mapped memory is a mesh's, written once: cached like an upload.
        std::shared_lock lock(g_mu);
        const auto cached = g_hashes->find({va, size});
        if (cached != g_hashes->end() && cached->second) return cached->second;
    }
    const uint32_t hash = Crc32c(0, data, static_cast<size_t>(size));
    std::unique_lock lock(g_mu);
    (*g_hashes)[{va, size}] = hash;
    g_maxRegion = std::max(g_maxRegion, size);
    return hash;
}

Stats GetStats() {
    std::shared_lock lock(g_mu);
    return {g_kept->size(), static_cast<size_t>(g_keptBytes), g_hashes->size(), static_cast<size_t>(g_misses)};
}

}  // namespace efmivulkan::ibregion
