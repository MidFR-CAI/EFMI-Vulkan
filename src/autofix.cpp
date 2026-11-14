// Auto-fix at runtime (autofix.h): the capture, then the analysis, the fixed inis and the
// report, on the job thread.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>

#include "autofix.h"
#include "common.h"
#include "efmi.h"
#include "efmidraw.h"
#include "log.h"
#include "modindex.h"
#include "textures.h"

namespace efmivulkan::autofix {
namespace {

// Mods off for kSettleFrames presents (what is on screen is the game's own), kRecordFrames
// recorded, then kReadFrames for the textures asked to be read back to be hashed.
enum class Phase { kIdle, kSettling, kRecording, kReading, kAnalysing };
constexpr int kSettleFrames = 4, kRecordFrames = 2, kReadFrames = 90;
constexpr size_t kMaxDraws = 100000, kMaxStored = 400000;

std::atomic<Phase> g_phase{Phase::kIdle};
std::atomic<bool> g_applyWanted{false};
std::atomic<bool> g_fixAfterCapture{false};  // RequestFix: apply as soon as the capture is kept.
int g_frames = 0;  // Present thread only.
std::mutex g_mu;
std::vector<CapturedDraw>* g_draws = new std::vector<CapturedDraw>();   // The capture under way.
std::vector<CapturedDraw>* g_stored = new std::vector<CapturedDraw>();  // The kept ones, numbered by capture.
uint32_t g_captures = 0;                                                // Kept, under g_mu.
std::atomic<std::shared_ptr<const Learned>> g_learned;

std::wstring SlotsPath() { return LogDir() + L"\\slots.ini"; }

bool ReadAll(const std::wstring& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool WriteAll(const std::wstring& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(f);
}

std::string Hex(uint32_t v) {
    char b[12];
    snprintf(b, sizeof(b), "%08x", v);
    return b;
}

std::wstring Stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[32];
    swprintf(b, 32, L"%04u-%02u-%02u %02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

// The kept captures (draws numbered by capture), and what they would fix. apply: the inis
// written and slots.ini added to; else a preview only.
void Run(std::vector<CapturedDraw> draws, uint32_t captures, bool apply) {
    const ModIndex* index = efmi::GetModIndex();
    std::vector<ModInfo> mods = efmidraw::DescribeMods();
    if (!index || mods.empty()) {
        LogError("auto-fix: the mods are not loaded yet");
        return;
    }
    // Hashes every installed mod names: never given to another.
    std::set<uint32_t> claimed;
    for (const auto& [hash, entries] : index->All()) claimed.insert(hash);
    // Textures read back since the capture.
    size_t images = 0, unread = 0;
    for (CapturedDraw& d : draws) {
        for (BoundImage& b : d.images) {
            ++images;
            if (!b.measured) b.measured = textures::MeasuredHashOf(b.imageId);
            unread += !b.measured;
        }
    }
    const Analysis a = Analyze(draws, mods, claimed, claimed);

    // The capture itself, to look into what the analysis made of it.
    FILE* raw = nullptr;
    _wfopen_s(&raw, (LogDir() + L"\\autofix-capture.log").c_str(), L"wb");
    if (raw) {
        fprintf(raw, "# hash count first colourTargets position | images: set.binding.element layout WxH format modHash measured\r\n");
        for (const CapturedDraw& d : draws) {
            fprintf(raw, "%08x %u %u %u %s%g %g %g\r\n", d.hash, d.indexCount, d.firstIndex, d.colorTargets, d.hasPosition ? "" : "(none) ", d.position[0],
                    d.position[1], d.position[2]);
            if (d.hasBone0) fprintf(raw, "  bone0 %g %g %g\r\n", d.bone0[0], d.bone0[1], d.bone0[2]);
            if (!d.pushed.empty()) {
                fprintf(raw, "  push");
                for (size_t k = 0; k + 4 <= d.pushed.size(); k += 4) {
                    uint32_t v;
                    memcpy(&v, d.pushed.data() + k, 4);
                    fprintf(raw, " %08x", v);
                }
                fprintf(raw, "\r\n");
            }
            for (const CapturedDraw::ConfigEntry& e : d.entries) {
                fprintf(raw, "  entry %u at %g %g %g bones %u\r\n", e.index, e.position[0], e.position[1], e.position[2], e.boneRow);
            }
            if (d.hasConfig) {
                for (int r = 0; r < 16; ++r) {
                    fprintf(raw, "  cfg%-2d %g %g %g %g\r\n", r, d.config[r * 4], d.config[r * 4 + 1], d.config[r * 4 + 2], d.config[r * 4 + 3]);
                }
            }
            for (const BoundImage& b : d.images) {
                fprintf(raw, "    %u.%u.%u %016llx %ux%u %u %08x %08x\r\n", b.at.set, b.at.binding, b.at.element, static_cast<unsigned long long>(b.at.layoutSig),
                        b.width, b.height, b.vkFormat, b.modHash, b.measured);
            }
        }
        fclose(raw);
    }

    std::string report = "efmivulkan auto-fix, " + ToUtf8(Stamp()) + (apply ? ": APPLIED\r\n" : ": preview, nothing written yet\r\n");
    char line[320];
    snprintf(line, sizeof(line), "%u captures kept: %zu draws of the sizes mods use, %zu textures bound (%zu never read)\r\n", captures, draws.size(), images,
             unread);
    report += line;
    report += apply ? "\r\n"
                    : "Ctrl+F10 again with other characters in view adds a capture; Ctrl+Shift+F10 (or Shift+F8) fixes everything below.\r\n\r\n";
    size_t fixedMods = 0, offScreen = 0;
    std::string notShown;
    for (const ModResult& r : a.mods) {
        const ModInfo& mod = mods[r.mod];
        if (!r.onScreen) {
            ++offScreen;
            notShown += "  " + mod.name + "\r\n";
            continue;
        }
        report += "== " + mod.name + "\r\n";
        for (const Remap& m : r.ib) report += "  mesh    [" + m.section + "] " + Hex(m.from) + " -> " + Hex(m.to) + "  (" + m.why + ")\r\n";
        for (const Remap& m : r.textures) report += "  texture " + Hex(m.from) + " -> " + Hex(m.to) + "  (" + m.why + ")\r\n";
        for (const std::string& n : r.notes) report += "  left:   " + n + "\r\n";
        if (r.ib.empty() && r.textures.empty()) {
            report += r.notes.empty() ? "  nothing to fix: every part and texture is the game's\r\n" : "";
            continue;
        }
        std::string text;
        if (!ReadAll(mod.ini, &text)) {
            report += "  could not read " + ToUtf8(mod.ini) + "\r\n";
            continue;
        }
        size_t changed = 0;
        const std::string fixed = RewriteIni(text, r.ib, r.textures, &changed);
        if (!changed) {
            report += "  no hash line to change was found in " + ToUtf8(mod.ini) + "\r\n";
            continue;
        }
        if (!apply) {
            ++fixedMods;
            report += "  to write: " + std::to_string(changed) + " lines\r\n";
            continue;
        }
        const std::wstring backup = mod.ini + L" " + Stamp() + L".BAK";
        if (!CopyFileW(mod.ini.c_str(), backup.c_str(), TRUE) || !WriteAll(mod.ini, fixed)) {
            report += "  could not write " + ToUtf8(mod.ini) + " (error " + std::to_string(GetLastError()) + "); nothing changed\r\n";
            continue;
        }
        ++fixedMods;
        report += "  written: " + std::to_string(changed) + " lines; the original is " + ToUtf8(backup) + "\r\n";
    }
    if (offScreen) report += "\r\nnot on screen, so not looked at (capture again with them in view):\r\n" + notShown;

    // Slots: the captures' votes added to the ones before (one vote per component and place,
    // however many captures show it), when applied.
    std::string slotsText;
    ReadAll(SlotsPath(), &slotsText);
    SlotVotes votes = ParseSlots(slotsText);
    AddVotes(&votes, a.votes);
    if (apply) {
        WriteAll(SlotsPath(), FormatSlots(votes));
        LoadLearnedSlots();
    }
    const auto learned = std::make_shared<const Learned>(Decide(votes));
    report += "\r\ntexture registers (ps-tN) placed on Vulkan: " + std::to_string(learned->size()) + " of " + std::to_string(votes.size()) +
              " seen (a place per shader layout, only where no other register claims it)";
    for (const auto& [slot, places] : *learned) {
        snprintf(line, sizeof(line), "%s t%d in %zu layouts", slot == learned->begin()->first ? ":" : ",", slot, places.size());
        report += line;
    }
    report += "\r\n";
    if (fixedMods && apply) {
        efmidraw::Reload();  // The fixed inis, in game now.
        report += "\r\n" + std::to_string(fixedMods) + " mods fixed and reloaded.\r\n";
    }
    if (fixedMods && !apply) report += "\r\n" + std::to_string(fixedMods) + " mods to fix: Ctrl+Shift+F10 writes them.\r\n";

    FILE* f = nullptr;
    _wfopen_s(&f, (LogDir() + L"\\autofix.log").c_str(), L"wb");
    if (f) {
        fwrite(report.data(), 1, report.size(), f);
        fclose(f);
    }
    LogInfo("auto-fix %s: %u captures, %zu draws, %zu mods %s, %zu not on screen, %zu texture registers placed; see autofix.log", apply ? "applied" : "preview",
            captures, draws.size(), fixedMods, apply ? "fixed" : "to fix", offScreen, learned->size());
    MessageBeep(apply ? (fixedMods ? MB_OK : MB_ICONHAND) : MB_ICONASTERISK);
}

}  // namespace

void RequestCapture() {
    Phase idle = Phase::kIdle;
    if (g_phase.compare_exchange_strong(idle, Phase::kSettling)) {
        LogInfo("auto-fix: mods off for a moment, capturing the game's own characters");
    }
}

void RequestApply() {
    g_applyWanted.store(true);
    LogInfo("auto-fix: fixing from the kept captures");
}

void RequestFix() {
    g_fixAfterCapture.store(true);
    RequestCapture();
}

bool ModsOff() {
    const Phase p = g_phase.load(std::memory_order_relaxed);
    return p == Phase::kSettling || p == Phase::kRecording;
}

bool Recording() { return g_phase.load(std::memory_order_relaxed) == Phase::kRecording; }

void OnDraw(CapturedDraw&& draw) {
    std::lock_guard lock(g_mu);
    if (g_draws->size() < kMaxDraws) g_draws->push_back(std::move(draw));
}

void OnPresent() {
    switch (g_phase.load()) {
        case Phase::kIdle: {
            // Apply, once no capture is under way: every kept capture, then they are let go.
            if (!g_applyWanted.exchange(false)) return;
            // Kept after the fix too: a later capture adds to them, and one that saw a character
            // only in part is completed by another.
            std::vector<CapturedDraw> draws;
            uint32_t captures = 0;
            {
                std::lock_guard lock(g_mu);
                draws = *g_stored;
                captures = g_captures;
            }
            if (!captures) {
                LogInfo("auto-fix: no captures kept; Shift+F8 with the broken characters in view");
                MessageBeep(MB_ICONHAND);
                return;
            }
            g_phase.store(Phase::kAnalysing);
            PostJob([draws = std::move(draws), captures]() mutable {
                Guard("auto-fix", [&] { Run(std::move(draws), captures, true); });
                g_phase.store(Phase::kIdle);
            });
            return;
        }
        case Phase::kSettling:
            if (++g_frames < kSettleFrames) return;
            {
                std::lock_guard lock(g_mu);
                g_draws->clear();
            }
            g_frames = 0;
            g_phase.store(Phase::kRecording);
            return;
        case Phase::kRecording:
            if (++g_frames < kRecordFrames) return;
            g_frames = 0;
            g_phase.store(Phase::kReading);  // Mods back on.
            return;
        case Phase::kReading: {
            if (++g_frames < kReadFrames) return;
            g_frames = 0;
            // Kept with the others; the preview covers all of them.
            std::vector<CapturedDraw> all;
            uint32_t captures = 0;
            {
                std::lock_guard lock(g_mu);
                const uint32_t number = g_captures++;
                for (CapturedDraw& d : *g_draws) {
                    if (g_stored->size() >= kMaxStored) break;
                    d.capture = number;
                    // Read back by now: kept, as the game may let the image go before the fix.
                    for (BoundImage& b : d.images) {
                        if (!b.measured) b.measured = textures::MeasuredHashOf(b.imageId);
                    }
                    g_stored->push_back(std::move(d));
                }
                g_draws->clear();
                all = *g_stored;
                captures = g_captures;
            }
            if (g_fixAfterCapture.exchange(false)) {
                // Shift+F8: straight to the fix (the idle phase applies it), no preview.
                g_applyWanted.store(true);
                g_phase.store(Phase::kIdle);
                return;
            }
            g_phase.store(Phase::kAnalysing);
            PostJob([all = std::move(all), captures]() mutable {
                Guard("auto-fix", [&] { Run(std::move(all), captures, false); });
                g_phase.store(Phase::kIdle);
            });
            return;
        }
        default:
            return;
    }
}

std::shared_ptr<const Learned> LearnedSlots() { return g_learned.load(std::memory_order_acquire); }

void LoadLearnedSlots() {
    std::string text;
    if (!ReadAll(SlotsPath(), &text)) return;
    auto decided = std::make_shared<const Learned>(Decide(ParseSlots(text)));
    if (!decided->empty()) LogInfo("auto-fix: %zu texture registers (ps-tN) placed on Vulkan, from slots.ini", decided->size());
    g_learned.store(std::move(decided), std::memory_order_release);
}

}  // namespace efmivulkan::autofix
