// Index of the hashes referenced by the installed mods. Follows d3dx.ini's
// include rules: every *.ini under Mods, skipping DISABLED* and desktop.ini.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace efmivulkan {

// Vb0Component is WWMI's: EFMI keys components on index buffer regions instead.
enum class HashKind { IbComponent, Vb0Component, ShapeKey, ConstantBuffer, Texture, Other };
const char* HashKindName(HashKind k);

struct ModEntry {
    uint32_t hash = 0;
    HashKind kind = HashKind::Other;
    std::string file;     // Relative to the Mods folder.
    std::string section;
    std::optional<uint32_t> matchFirstIndex, matchIndexCount;
};

class ModIndex {
public:
    void Load(const std::wstring& modsDir);

    bool Has(uint32_t hash) const { return byHash_.count(hash) != 0; }
    // Whether any EFMI component matches draws of this many indices: the cheap test a
    // draw passes before its index region is hashed.
    bool HasIndexCount(uint32_t count) const { return indexCounts_.count(count) != 0; }
    size_t IndexCountCount() const { return indexCounts_.size(); }
    const std::vector<ModEntry>& Entries(uint32_t hash) const;
    const std::map<uint32_t, std::vector<ModEntry>>& All() const { return sorted_; }
    size_t IniCount() const { return iniCount_; }
    const std::wstring& Dir() const { return dir_; }

private:
    void ParseFile(const std::wstring& path, const std::string& relative);
    void Walk(const std::wstring& dir, const std::string& relative);

    std::wstring dir_;
    size_t iniCount_ = 0;
    std::map<uint32_t, std::vector<ModEntry>> sorted_;
    std::unordered_map<uint32_t, const std::vector<ModEntry>*> byHash_;
    std::unordered_set<uint32_t> indexCounts_;
};

// Default: the EFMI folder of this XXMI install. Override with EFMIVULKAN_MODS.
std::wstring DefaultModsDir();

}  // namespace efmivulkan
