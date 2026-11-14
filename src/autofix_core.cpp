// The pure half of auto-fix (autofix.h): the analysis, ini rewriting, slots.ini.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <tuple>

#include "autofix.h"

namespace efmivulkan::autofix {
namespace {

// Draws of one character share their bone config's position (EFMI's spatial identification
// treats 3 cells of 0.125 as the same object).
constexpr float kGroupRadius = 0.375f;

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Hex(uint32_t v) {
    char b[12];
    snprintf(b, sizeof(b), "%08x", v);
    return b;
}

bool IsLod(const ModComponent& c) { return c.lod > 0 || Lower(c.section).find("_lod") != std::string::npos; }

// The same texture up to sRGB: an export's DDS and the game's image may differ in it.
uint32_t FormatFamily(uint32_t vk) {
    switch (vk) {
        case 134: return 133;  // BC1 sRGB.
        case 136: return 135;  // BC2.
        case 138: return 137;  // BC3.
        case 146: return 145;  // BC7.
        case 43: return 37;    // R8G8B8A8.
        case 50: return 44;    // B8G8R8A8.
        case 132: return 131;  // BC1 RGB.
        default: return vk;
    }
}

struct Group {
    float center[3] = {};
    uint32_t capture = 0;
    std::vector<size_t> draws;
    std::map<uint32_t, std::set<uint32_t>> byCount;  // Index count -> hashes.
    std::set<uint32_t> hashes;
    std::map<uint32_t, bool> inColourPass;  // Hash -> drawn in a pass with colour targets.
};

// Per capture: characters by position. Captures never share a group: one character swapped
// for another at the same place is two characters.
std::vector<Group> GroupDraws(const std::vector<CapturedDraw>& draws) {
    std::vector<Group> groups;
    std::map<uint32_t, Group> loose;  // Per capture, draws with no bone config: a group of their own, last.
    for (size_t i = 0; i < draws.size(); ++i) {
        const CapturedDraw& d = draws[i];
        Group* g = &loose[d.capture];
        if (d.hasPosition) {
            g = nullptr;
            for (Group& k : groups) {
                if (k.capture != d.capture) continue;
                const float dx = k.center[0] - d.position[0], dy = k.center[1] - d.position[1], dz = k.center[2] - d.position[2];
                if (dx * dx + dy * dy + dz * dz <= kGroupRadius * kGroupRadius) {
                    g = &k;
                    break;
                }
            }
            if (!g) {
                groups.emplace_back();
                g = &groups.back();
                g->capture = d.capture;
                std::copy(d.position, d.position + 3, g->center);
            }
        }
        g->draws.push_back(i);
        if (!d.hash) continue;
        g->byCount[d.indexCount].insert(d.hash);
        g->hashes.insert(d.hash);
        g->inColourPass[d.hash] = g->inColourPass[d.hash] || d.colorTargets > 0;
    }
    for (auto& [capture, g] : loose) {
        g.capture = capture;
        if (!g.draws.empty()) groups.push_back(std::move(g));
    }
    return groups;
}

}  // namespace

std::vector<int> ComponentIdsOfName(const std::string& fileName) {
    std::vector<int> ids;
    const std::string lower = Lower(fileName);
    const size_t slash = lower.find_last_of("/\\");
    size_t at = lower.find("components-", slash == std::string::npos ? 0 : slash + 1);
    if (at == std::string::npos) return ids;
    at += 11;
    while (at < lower.size() && std::isdigit(static_cast<unsigned char>(lower[at]))) {
        ids.push_back(std::atoi(lower.c_str() + at));
        while (at < lower.size() && std::isdigit(static_cast<unsigned char>(lower[at]))) ++at;
        if (at < lower.size() && lower[at] == '-') ++at;
    }
    return ids;
}

Analysis Analyze(const std::vector<CapturedDraw>& draws, const std::vector<ModInfo>& mods, const std::set<uint32_t>& claimedIb,
                 const std::set<uint32_t>& claimedTextures) {
    Analysis out;
    const std::vector<Group> groups = GroupDraws(draws);
    std::set<uint32_t> takenIb = claimedIb, takenTextures = claimedTextures;  // Grows as hashes are given out.

    for (size_t m = 0; m < mods.size(); ++m) {
        const ModInfo& mod = mods[m];
        ModResult r;
        r.mod = m;
        if (mod.components.empty()) continue;

        // The character: the group with most of the mod's components, by hash or index count.
        std::vector<std::pair<size_t, size_t>> fit(groups.size());  // Per group: (matched, score).
        for (size_t g = 0; g < groups.size(); ++g) {
            size_t& matched = fit[g].first;
            size_t& score = fit[g].second;
            for (const ModComponent& c : mod.components) {
                if (groups[g].hashes.count(c.hash)) {
                    ++matched;
                    ++score;
                    continue;
                }
                const auto it = groups[g].byCount.find(c.indexCount);
                if (it == groups[g].byCount.end()) continue;
                for (uint32_t h : it->second) {
                    if (!takenIb.count(h)) {
                        ++score;
                        break;
                    }
                }
            }
        }
        int best = -1;
        for (size_t g = 0; g < groups.size(); ++g) {
            if (best < 0 || fit[g] > fit[best]) best = static_cast<int>(g);
        }
        const size_t bestMatched = best < 0 ? 0 : fit[best].first, bestScore = best < 0 ? 0 : fit[best].second;
        size_t secondScore = 0;
        for (size_t g = 0; g < groups.size(); ++g) {
            if (static_cast<int>(g) != best) secondScore = std::max(secondScore, fit[g].second);
        }
        std::set<uint32_t> counts;
        for (const ModComponent& c : mod.components) counts.insert(c.indexCount);
        const size_t needed = std::max<size_t>(2, (counts.size() * 3 + 9) / 10);
        if (best < 0 || (bestMatched == 0 && (bestScore < needed || bestScore == secondScore))) {
            r.notes.push_back(best >= 0 && bestScore >= needed
                                  ? "two characters on screen fit it equally: capture with only this one in view"
                                  : "its character is not on screen (" + std::to_string(bestScore) + " of " + std::to_string(mod.components.size()) +
                                        " parts found by size): capture with it in view");
            out.mods.push_back(std::move(r));
            continue;
        }
        r.onScreen = true;
        const Group& g = groups[best];

        // Meshes.
        std::map<size_t, uint32_t> newHash;  // Component -> its hash now.
        std::map<uint32_t, std::vector<size_t>> wanted;  // Candidate hash -> components that would take it.
        for (size_t k = 0; k < mod.components.size(); ++k) {
            const ModComponent& c = mod.components[k];
            if (g.hashes.count(c.hash)) {
                newHash[k] = c.hash;
                continue;
            }
            const auto it = g.byCount.find(c.indexCount);
            std::vector<uint32_t> free;
            if (it != g.byCount.end()) {
                for (uint32_t h : it->second) {
                    if (!takenIb.count(h)) free.push_back(h);
                }
            }
            if (free.size() > 1) {
                // LoD 1 meshes are what shadow passes draw; LoD 0 is what the camera sees.
                std::vector<uint32_t> lodFit;
                for (uint32_t h : free) {
                    if (g.inColourPass.at(h) != IsLod(c)) lodFit.push_back(h);
                }
                if (!lodFit.empty()) free = lodFit;
            }
            // A LoD the game did not draw this frame (LoD 1 up close), while another LoD of the
            // same component matches: nothing is wrong with it.
            bool otherLodDrawn = false;
            for (const ModComponent& o : mod.components) {
                otherLodDrawn |= &o != &c && o.componentId >= 0 && o.componentId == c.componentId && g.hashes.count(o.hash);
            }
            if (otherLodDrawn) continue;  // Its other LoD is the game's still: this one most likely too.
            if (free.size() == 1) {
                wanted[free[0]].push_back(k);
            } else if (free.empty()) {
                r.notes.push_back(c.section + ": no draw of " + std::to_string(c.indexCount) +
                                  " indices on its character; the game changed this mesh (the mod needs a new export) or it is not in view");
            } else {
                r.notes.push_back(c.section + ": " + std::to_string(free.size()) + " draws of " + std::to_string(c.indexCount) +
                                  " indices fit; left as it is");
            }
        }
        for (const auto& [h, ks] : wanted) {
            if (ks.size() == 1) {
                const ModComponent& c = mod.components[ks[0]];
                r.ib.push_back({c.section, c.hash, h, "same index count (" + std::to_string(c.indexCount) + ") on the same character"});
                newHash[ks[0]] = h;
                takenIb.insert(h);
                continue;
            }
            for (size_t k : ks) r.notes.push_back(mod.components[k].section + ": shares its only candidate " + Hex(h) + " with another part; left as it is");
        }

        // The draws of each component, now.
        std::vector<std::vector<size_t>> componentDraws(mod.components.size());
        for (size_t i : g.draws) {
            for (const auto& [k, h] : newHash) {
                if (draws[i].hash == h && draws[i].indexCount == mod.components[k].indexCount) componentDraws[k].push_back(i);
            }
        }

        // Textures.
        std::map<size_t, uint32_t> textureNow;  // Texture -> the game hash it stands for now.
        for (size_t t = 0; t < mod.textures.size(); ++t) {
            const ModTexture& tex = mod.textures[t];
            std::set<uint32_t> own(tex.iniHashes.begin(), tex.iniHashes.end());
            if (tex.fileHash) own.insert(tex.fileHash);
            bool bound = false;
            for (size_t k = 0; k < mod.components.size() && !bound; ++k) {
                for (size_t i : componentDraws[k]) {
                    for (const BoundImage& b : draws[i].images) bound |= own.count(b.modHash) || own.count(b.measured);
                }
            }
            if (tex.everMatched || bound) {
                if (tex.fileHash) textureNow[t] = tex.fileHash;
                continue;
            }
            if (!tex.width || own.empty()) continue;
            std::set<uint32_t> candidates;
            bool smaller = false, unread = false, anyDraw = false;
            for (size_t k = 0; k < mod.components.size(); ++k) {
                const ModComponent& c = mod.components[k];
                if (!tex.componentIds.empty() && std::find(tex.componentIds.begin(), tex.componentIds.end(), c.componentId) == tex.componentIds.end()) {
                    continue;
                }
                for (size_t i : componentDraws[k]) {
                    if (!draws[i].colorTargets) continue;
                    anyDraw = true;
                    for (const BoundImage& b : draws[i].images) {
                        if (FormatFamily(b.vkFormat) != FormatFamily(tex.vkFormat) || b.modHash) continue;
                        if (b.width != tex.width || b.height != tex.height) {
                            smaller |= b.width * tex.height == b.height * tex.width && b.width < tex.width;
                            continue;
                        }
                        if (!b.measured) {
                            unread = true;
                            continue;
                        }
                        if (!takenTextures.count(b.measured)) candidates.insert(b.measured);
                    }
                }
            }
            if (candidates.size() == 1) {
                const uint32_t to = *candidates.begin();
                for (uint32_t from : own) {
                    r.textures.push_back({"", from, to, tex.file + ": the one game texture of its size and format in its parts' draws"});
                }
                textureNow[t] = to;
                takenTextures.insert(to);
            } else if (!anyDraw) {
                r.notes.push_back(tex.file + ": none of its parts was drawn in view; left as it is");
            } else if (candidates.empty()) {
                r.notes.push_back(tex.file + (unread ? ": its parts' textures of that size are not read back yet; capture again"
                                              : smaller ? ": only smaller streamed copies are bound; get closer and capture again"
                                                        : ": no game texture of its size and format in its parts' draws; left as it is"));
            } else {
                r.notes.push_back(tex.file + ": " + std::to_string(candidates.size()) + " game textures of its size and format fit; left as it is");
            }
        }

        // Slots: a ps-tN texture found bound in its own component's draw says where tN is. One
        // vote per component, slot and place, however many frames and passes showed it.
        std::set<std::tuple<size_t, int, SlotLocation>> voted;
        for (size_t k = 0; k < mod.components.size(); ++k) {
            for (const auto& [slot, t] : mod.components[k].slots) {
                if (t < 0 || static_cast<size_t>(t) >= mod.textures.size()) continue;
                const ModTexture& tex = mod.textures[t];
                std::set<uint32_t> own(tex.iniHashes.begin(), tex.iniHashes.end());
                if (tex.fileHash) own.insert(tex.fileHash);
                if (textureNow.count(t)) own.insert(textureNow[t]);
                own.erase(0);
                for (size_t i : componentDraws[k]) {
                    if (!draws[i].colorTargets) continue;
                    for (const BoundImage& b : draws[i].images) {
                        if ((own.count(b.modHash) || own.count(b.measured)) && voted.insert({k, slot, b.at}).second) ++out.votes[slot][b.at];
                    }
                }
            }
        }
        out.mods.push_back(std::move(r));
    }
    return out;
}

std::string RewriteIni(const std::string& text, const std::vector<Remap>& ib, const std::vector<Remap>& textures, size_t* changed) {
    std::set<std::string> ibSections;
    for (const Remap& r : ib) ibSections.insert(Lower(r.section));
    std::string out;
    out.reserve(text.size());
    std::string section;
    size_t count = 0;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        end = end == std::string::npos ? text.size() : end + 1;
        std::string line = text.substr(at, end - at);
        at = end;
        size_t p = line.find_first_not_of(" \t");
        if (p != std::string::npos && line[p] == '[') {
            const size_t close = line.find(']', p);
            if (close != std::string::npos) section = Lower(line.substr(p + 1, close - p - 1));
        } else if (p != std::string::npos && Lower(line.substr(p, 4)) == "hash") {
            size_t q = line.find_first_not_of(" \t", p + 4);
            if (q != std::string::npos && line[q] == '=') {
                q = line.find_first_not_of(" \t", q + 1);
                size_t e = q;
                while (e != std::string::npos && e < line.size() && std::isxdigit(static_cast<unsigned char>(line[e]))) ++e;
                if (q != std::string::npos && e - q == 8) {
                    const uint32_t value = static_cast<uint32_t>(std::strtoul(line.substr(q, 8).c_str(), nullptr, 16));
                    const uint32_t* to = nullptr;
                    for (const Remap& r : ib) {
                        if (r.from == value && Lower(r.section) == section) to = &r.to;
                    }
                    if (!to && !ibSections.count(section)) {
                        for (const Remap& r : textures) {
                            if (r.from == value) to = &r.to;
                        }
                    }
                    if (to) {
                        line.replace(q, 8, Hex(*to));
                        ++count;
                    }
                }
            }
        }
        out += line;
    }
    if (changed) *changed = count;
    return out;
}

SlotVotes ParseSlots(const std::string& text) {
    SlotVotes votes;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == ';' || (line[0] != 't' && line[0] != 'T')) continue;
        int slot = 0;
        unsigned set = 0, binding = 0, element = 0, n = 0;
        unsigned long long sig = 0;
        if (sscanf_s(line.c_str() + 1, "%d = %u %u %u %llx %u", &slot, &set, &binding, &element, &sig, &n) == 6) {
            votes[slot][{set, binding, element, sig}] += n;
        }
    }
    return votes;
}

std::string FormatSlots(const SlotVotes& votes) {
    std::string out =
        "; EFMIVulkan: where DX11 texture registers (ps-tN) are on Vulkan, learned by auto-fix (Shift+F8).\r\n"
        "; t<N> = <set> <binding> <element> <set layout> <votes>\r\n";
    const Learned decided = Decide(votes);
    for (const auto& [slot, places] : votes) {
        for (const auto& [at, n] : places) {
            char b[160];
            const auto d = decided.find(slot);
            const bool used = d != decided.end() && std::find(d->second.begin(), d->second.end(), at) != d->second.end();
            snprintf(b, sizeof(b), "t%d = %u %u %u %016llx %u%s\r\n", slot, at.set, at.binding, at.element, static_cast<unsigned long long>(at.layoutSig), n,
                     used ? "  ; used" : "");
            out += b;
        }
    }
    return out;
}

void AddVotes(SlotVotes* into, const SlotVotes& more) {
    for (const auto& [slot, places] : more) {
        for (const auto& [at, n] : places) (*into)[slot][at] += n;
    }
}

Learned Decide(const SlotVotes& votes) {
    // Who else claims a place: a binding two registers vote for (2 votes or more each) is
    // where a texture happens to be, not where a register is.
    std::map<SlotLocation, int> claims;
    for (const auto& [slot, places] : votes) {
        for (const auto& [at, n] : places) claims[at] += n >= 2;
    }
    Learned out;
    for (const auto& [slot, places] : votes) {
        std::map<uint64_t, std::vector<std::pair<SlotLocation, uint32_t>>> byLayout;
        for (const auto& [at, n] : places) byLayout[at.layoutSig].push_back({at, n});
        for (const auto& [layout, list] : byLayout) {
            uint32_t total = 0, top = 0;
            const SlotLocation* best = nullptr;
            for (const auto& [at, n] : list) {
                total += n;
                if (n > top) {
                    top = n;
                    best = &at;
                }
            }
            if (best && top >= 2 && top * 4 >= total * 3 && claims[*best] <= 1) out[slot].push_back(*best);
        }
    }
    return out;
}

}  // namespace efmivulkan::autofix
