#include "textures.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "log.h"
#include "texhash.h"

namespace efmivulkan::textures {
namespace {

std::mutex g_mu;
std::set<uint32_t>* g_targets = new std::set<uint32_t>();
std::unordered_map<uint64_t, uint32_t>* g_matched = new std::unordered_map<uint64_t, uint32_t>();  // Image id to hash.
std::set<uint32_t>* g_seen = new std::set<uint32_t>();                                            // Hashes logged once.
std::map<std::string, uint64_t>* g_variants = new std::map<std::string, uint64_t>();               // Which description matched.
std::atomic<uint64_t> g_uploads{0}, g_unknownFormat{0}, g_matches{0}, g_streamedMatches{0};

// Streamed copies (see OnMipUpload), under g_mu: by format, size and data hash of one level.
uint64_t LevelKey(uint32_t vkFormat, uint32_t width, uint32_t height, uint32_t dataHash) {
    uint64_t h = (uint64_t(vkFormat) << 48) ^ (uint64_t(width) << 32) ^ (uint64_t(height) << 16) ^ 0x9E3779B97F4A7C15ull;
    h = (h ^ dataHash) * 0xC2B2AE3D27D4EB4Full;
    return h ^ (h >> 29);
}
std::unordered_map<uint64_t, uint32_t>* g_lowerMips = new std::unordered_map<uint64_t, uint32_t>();  // A matched texture's lower mip -> its hash.
std::unordered_map<uint64_t, std::vector<uint64_t>>* g_unmatched = new std::unordered_map<uint64_t, std::vector<uint64_t>>();  // Mip 0 -> images.
constexpr size_t kMaxUnmatched = 200000;

void Matched(uint64_t imageId, uint32_t hash) {
    (*g_matched)[imageId] = hash;
    g_matches.fetch_add(1, std::memory_order_relaxed);
}
std::map<uint32_t, uint64_t>* g_unknownFormats = new std::map<uint32_t, uint64_t>();  // VkFormat -> uploads, under g_mu.

}  // namespace

void SetTargets(std::set<uint32_t> hashes) {
    std::lock_guard lock(g_mu);
    LogInfo("textures: %zu hashes named by mods", hashes.size());
    *g_targets = std::move(hashes);
}

void OnImageUpload(uint64_t imageId, uint32_t vkFormat, uint32_t width, uint32_t height, uint32_t mips, const uint8_t* mip0, uint64_t bytes) {
    g_uploads.fetch_add(1, std::memory_order_relaxed);
    const uint64_t expected = texhash::MipBytes(vkFormat, width, height);
    if (!expected) {
        g_unknownFormat.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(g_mu);
        ++(*g_unknownFormats)[vkFormat];
        return;
    }
    if (bytes < expected) return;
    // Staging memory is write-combined more often than not: read it the fast way.
    const uint32_t data = texhash::DataHash(mip0, expected, width, height, true);
    texhash::Candidate c = texhash::Hash(data, vkFormat, width, height, mips);
    std::lock_guard lock(g_mu);
    const uint64_t level = LevelKey(vkFormat, width, height, data);
    if (!g_targets->count(c.hash)) {
        // A streamed copy of a texture that matched already?
        const auto lower = g_lowerMips->find(level);
        if (lower != g_lowerMips->end()) {
            Matched(imageId, lower->second);
            if (g_streamedMatches.fetch_add(1, std::memory_order_relaxed) < 20) {
                LogInfo("texture MATCH %08x: a streamed copy, %ux%u", lower->second, width, height);
            }
            return;
        }
        // Not as measured: every other description the DX11 game might have used, with the
        // image's mip count, a full chain, or one mip.
        uint32_t full = 1;
        while ((std::max(width, height) >> full) > 0) ++full;
        bool found = false;
        for (const uint32_t m : {mips, full, 1u}) {
            for (const texhash::Candidate& k : texhash::Candidates(data, vkFormat, width, height, m)) {
                if (g_targets->count(k.hash)) {
                    c = k;
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        if (!found) {
            // Kept, in case it is a streamed copy of a texture that matches later.
            if (g_unmatched->size() >= kMaxUnmatched) g_unmatched->clear();
            (*g_unmatched)[level].push_back(imageId);
            return;
        }
    }
    (*g_matched)[imageId] = c.hash;
    ++(*g_variants)[texhash::Describe(c)];
    g_matches.fetch_add(1, std::memory_order_relaxed);
    if (g_seen->insert(c.hash).second) {
        LogInfo("texture MATCH %08x: %ux%u, %u mips, Vulkan format %u, as %s", c.hash, width, height, mips, vkFormat, texhash::Describe(c).c_str());
    }
}

std::atomic<uint64_t> g_matchedDestroyed{0};

void OnMipUpload(uint64_t imageId, uint32_t vkFormat, uint32_t mip, uint32_t width, uint32_t height, const uint8_t* data, uint64_t bytes) {
    if (mip == 0) return;
    const uint64_t expected = texhash::MipBytes(vkFormat, width, height);
    if (!expected || bytes < expected) return;
    uint32_t hash;
    {
        std::lock_guard lock(g_mu);
        const auto it = g_matched->find(imageId);
        if (it == g_matched->end()) return;
        hash = it->second;
    }
    const uint32_t dataHash = texhash::DataHash(data, expected, width, height, true);
    const uint64_t level = LevelKey(vkFormat, width, height, dataHash);
    std::lock_guard lock(g_mu);
    (*g_lowerMips)[level] = hash;
    // Copies that went up before this one did.
    const auto waiting = g_unmatched->find(level);
    if (waiting == g_unmatched->end()) return;
    for (const uint64_t id : waiting->second) {
        if (g_matched->count(id)) continue;
        Matched(id, hash);
        if (g_streamedMatches.fetch_add(1, std::memory_order_relaxed) < 20) {
            LogInfo("texture MATCH %08x: a streamed copy, %ux%u, that went up first", hash, width, height);
        }
    }
    g_unmatched->erase(waiting);
}

void OnImageDestroyed(uint64_t imageId) {
    std::lock_guard lock(g_mu);
    if (g_matched->erase(imageId)) g_matchedDestroyed.fetch_add(1, std::memory_order_relaxed);
}

uint32_t HashOf(uint64_t imageId) {
    std::lock_guard lock(g_mu);
    const auto it = g_matched->find(imageId);
    return it == g_matched->end() ? 0 : it->second;
}

void LogStats() {
    std::string variants, unknown;
    {
        std::lock_guard lock(g_mu);
        for (const auto& [format, count] : *g_unknownFormats) unknown += " " + std::to_string(format) + "x" + std::to_string(count);
        for (const auto& [name, count] : *g_variants) variants += "; " + name + " x" + std::to_string(count);
    }
    LogInfo("textures: %llu mip-0 uploads hashed (%llu in formats not handled), %llu matched a mod%s",
            (unsigned long long)g_uploads.load(), (unsigned long long)g_unknownFormat.load(), (unsigned long long)g_matches.load(),
            variants.c_str());
    LogInfo("textures: %llu matched game textures destroyed by the game since (streaming or unloading); %llu streamed copies matched",
            (unsigned long long)g_matchedDestroyed.load(), (unsigned long long)g_streamedMatches.load());
    if (!unknown.empty()) LogInfo("textures: uploads in formats not handled, by VkFormat:%s", unknown.c_str());
}

}  // namespace efmivulkan::textures
