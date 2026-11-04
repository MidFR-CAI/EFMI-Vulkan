#include "efmi.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "common.h"
#include "ibregion.h"
#include "log.h"
#include "modindex.h"

namespace efmivulkan::efmi {
namespace {

std::atomic<ModIndex*> g_mods{nullptr};
std::atomic<uint64_t> g_indexedDraws{0}, g_indirectDraws{0}, g_candidates{0}, g_matches{0};

// Capture: F10 arms it, the next present starts it, the one after ends it, unless that
// frame had no draws.
enum class Capture { kIdle, kArmed, kRecording };
constexpr int kMaxEmptyFrames = 120;
std::atomic<Capture> g_capture{Capture::kIdle};
std::mutex g_captureMu;
struct CapturedDraw {
    IndexedDraw draw;
    uint32_t hash;
};
std::vector<CapturedDraw>* g_captured = new std::vector<CapturedDraw>();

std::mutex g_loggedMu;
std::set<uint32_t>* g_logged = new std::set<uint32_t>();

uint32_t HashOf(const IndexedDraw& d) {
    const uint64_t key = d.key + uint64_t(d.indexBytes) * d.firstIndex;
    const uint64_t size = uint64_t(d.indexBytes) * d.indexCount;
    if (d.mapped) return ibregion::RegionHashOf(key, d.mapped + uint64_t(d.indexBytes) * d.firstIndex, size);
    return ibregion::RegionHash(key, size);
}

std::string ModName(const std::string& file) { return file.substr(0, file.find('/')); }

FILE* OpenLog(const wchar_t* name) {
    FILE* f = nullptr;
    _wfopen_s(&f, (LogDir() + L"\\" + name).c_str(), L"wb");
    return f;
}

void WriteReports(const std::vector<CapturedDraw>& frame) {
    if (FILE* f = OpenLog(L"frame.log")) {
        fprintf(f, "# one line per vkCmdDrawIndexed in the frame, in recording order\r\n");
        fprintf(f, "# ibregion: the XXMI fork's region hash of the indices it reads, 0 if its data was never seen\r\n");
        for (size_t i = 0; i < frame.size(); ++i) {
            const IndexedDraw& d = frame[i].draw;
            fprintf(f, "%zu indices=%u first=%u vertexOffset=%d instances=%u firstInstance=%u index=%ubit buffer=%u+%llu%s ibregion=%08x\r\n", i,
                    d.indexCount, d.firstIndex, d.vertexOffset, d.instanceCount, d.firstInstance, d.indexBytes * 8,
                    static_cast<uint32_t>(d.key >> 36), static_cast<unsigned long long>(d.key & ((1ull << 36) - 1)),
                    d.mapped ? " mapped" : "", frame[i].hash);
        }
        fclose(f);
    }
    const ModIndex* mods = GetModIndex();
    FILE* f = OpenLog(L"crossref.log");
    if (!f) return;
    fprintf(f, "efmivulkan cross-reference\r\n");
    if (mods) fprintf(f, "mods: %zu ini files, %zu unique hashes, in %s\r\n", mods->IniCount(), mods->All().size(), ToUtf8(mods->Dir()).c_str());
    size_t unseen = 0;
    for (const auto& c : frame) unseen += c.hash == 0;
    fprintf(f, "frame: %zu indexed draws, %zu with index data never seen\r\n", frame.size(), unseen);
    if (!mods) {
        fclose(f);
        return;
    }

    // Each component: matched, its index count drawn with other bytes, or not drawn.
    std::map<uint32_t, std::vector<size_t>> byIndexCount;
    for (size_t i = 0; i < frame.size(); ++i) byIndexCount[frame[i].draw.indexCount].push_back(i);
    fprintf(f, "\r\n== EFMI COMPONENTS (index region hash + index count) ==\r\n");
    std::map<std::string, std::vector<const ModEntry*>> byFile;
    for (const auto& [hash, entries] : mods->All()) {
        for (const auto& e : entries) {
            if (e.kind == HashKind::IbComponent && e.matchIndexCount) byFile[e.file].push_back(&e);
        }
    }
    for (const auto& [file, entries] : byFile) {
        size_t matched = 0, drawn = 0;
        std::string lines;
        for (const ModEntry* e : entries) {
            const auto it = byIndexCount.find(*e->matchIndexCount);
            size_t hits = 0;
            std::set<uint32_t> others;
            if (it != byIndexCount.end()) {
                for (size_t i : it->second) {
                    if (frame[i].hash == e->hash) {
                        ++hits;
                    } else {
                        others.insert(frame[i].hash);
                    }
                }
            }
            char line[512];
            if (hits || !others.empty()) ++drawn;
            if (hits) {
                ++matched;
                snprintf(line, sizeof(line), "  %08x count=%u MATCH in %zu draws  [%s]\r\n", e->hash, *e->matchIndexCount, hits,
                         e->section.c_str());
            } else if (!others.empty()) {
                std::string seen;
                for (uint32_t h : others) {
                    char x[12];
                    snprintf(x, sizeof(x), "%s%08x", seen.empty() ? "" : ",", h);
                    if (seen.size() < 60) seen += x;
                }
                snprintf(line, sizeof(line), "  %08x count=%u drawn, but its bytes hash to %s (0 = data never seen)  [%s]\r\n", e->hash,
                         *e->matchIndexCount, seen.c_str(), e->section.c_str());
            } else {
                snprintf(line, sizeof(line), "  %08x count=%u not drawn  [%s]\r\n", e->hash, *e->matchIndexCount, e->section.c_str());
            }
            lines += line;
        }
        fprintf(f, "%s: %zu/%zu components matched\r\n%s", ModName(file).c_str(), matched, entries.size(), drawn ? lines.c_str() : "");
    }
    fclose(f);
}

}  // namespace

void LoadModIndex() {
    ModIndex* mods = new ModIndex();
    mods->Load(DefaultModsDir());
    LogInfo("mods: %zu ini files, %zu unique hashes, %zu component index counts, in %s", mods->IniCount(), mods->All().size(),
            mods->IndexCountCount(), ToUtf8(mods->Dir()).c_str());
    if (mods->All().empty()) LogError("no mod hashes found; set EFMIVULKAN_MODS to the Mods folder");
    g_mods.store(mods, std::memory_order_release);
}

const ModIndex* GetModIndex() { return g_mods.load(std::memory_order_acquire); }

uint32_t OnIndexedDraw(const IndexedDraw& d) {
    g_indexedDraws.fetch_add(1, std::memory_order_relaxed);
    const bool recording = g_capture.load(std::memory_order_relaxed) == Capture::kRecording;
    const ModIndex* mods = GetModIndex();
    const bool candidate = mods && mods->HasIndexCount(d.indexCount);
    if (!candidate && !recording) return 0;  // The common case: one set lookup.

    const uint32_t hash = d.indexBytes ? HashOf(d) : 0;
    if (recording) {
        std::lock_guard lock(g_captureMu);
        g_captured->push_back({d, hash});
    }
    if (!candidate) return 0;
    g_candidates.fetch_add(1, std::memory_order_relaxed);
    if (!hash) return 0;
    const bool match = mods->Has(hash);
    if (match) g_matches.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(g_loggedMu);
    if (!g_logged->insert(hash).second) return match ? hash : 0;
    if (match) {
        const ModEntry& e = mods->Entries(hash).front();
        LogInfo("ib MATCH %08x count=%u first=%u instances=%u: %s [%s]", hash, d.indexCount, d.firstIndex, d.instanceCount, e.file.c_str(),
                e.section.c_str());
    } else if (g_logged->size() <= 400) {
        LogInfo("ib candidate %08x count=%u first=%u: no mod has this hash", hash, d.indexCount, d.firstIndex);
    }
    return match ? hash : 0;
}

void OnIndirectDraw() { g_indirectDraws.fetch_add(1, std::memory_order_relaxed); }

void OnPresent() {
    g_stats.presents.fetch_add(1, std::memory_order_relaxed);
    Capture state = g_capture.load();
    if (state == Capture::kArmed) {
        std::lock_guard lock(g_captureMu);
        g_captured->clear();
        g_capture.store(Capture::kRecording);
    } else if (state == Capture::kRecording) {
        // A frame with no draws (a loading screen, a paused game) is not worth a report:
        // keep recording into the next one, for up to kMaxEmptyFrames.
        static int emptyFrames = 0;
        std::vector<CapturedDraw> frame;
        {
            std::lock_guard lock(g_captureMu);
            if (g_captured->empty() && ++emptyFrames < kMaxEmptyFrames) return;
            frame.swap(*g_captured);
        }
        emptyFrames = 0;
        g_capture.store(Capture::kIdle);
        PostJob([frame = std::move(frame)] { Guard("WriteReports", [&] { WriteReports(frame); }); });
    }
}

void RequestCapture() {
    Capture idle = Capture::kIdle;
    g_capture.compare_exchange_strong(idle, Capture::kArmed);
}

void LogStats() {
    const auto ib = ibregion::GetStats();
    LogInfo("stats: presents=%llu indexed draws=%llu indirect=%llu; draws with a mod index count %llu, region matches %llu; "
            "kept uploads %zu (%zu MB), region hashes %zu, never seen %zu",
            (unsigned long long)g_stats.presents.load(), (unsigned long long)g_indexedDraws.load(), (unsigned long long)g_indirectDraws.load(),
            (unsigned long long)g_candidates.load(), (unsigned long long)g_matches.load(), ib.keptBuffers, ib.keptBytes >> 20, ib.cachedHashes,
            ib.misses);
}

}  // namespace efmivulkan::efmi
