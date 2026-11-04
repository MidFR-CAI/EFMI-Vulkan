#include "modindex.h"
#include "settings.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "log.h"

namespace efmivulkan {
namespace {

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

bool StartsWithI(const std::wstring& s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0;
}

HashKind Classify(const std::string& section, bool hasMatch) {
    const std::string s = Lower(section);
    if (s.find("shapekey") != std::string::npos) return HashKind::ShapeKey;
    if (s.find("bonedata") != std::string::npos || s.find("markbone") != std::string::npos) return HashKind::ConstantBuffer;
    // EFMI fires overrides from the index buffer (CheckTextureOverride = ib), hashing the
    // region the draw reads, and filters components by index count.
    if (hasMatch || s.find("component") != std::string::npos) return HashKind::IbComponent;
    if (s.find("texture") != std::string::npos) {
        const std::string rest = s.substr(s.find("texture") + 7);
        if (rest.find("texture") != std::string::npos || !rest.empty()) return HashKind::Texture;
    }
    return HashKind::Other;
}

std::optional<uint32_t> ParseUint(const std::string& s) {
    try {
        return static_cast<uint32_t>(std::stoul(s));
    } catch (...) {
        return std::nullopt;
    }
}

std::string ReadText(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    std::string data(static_cast<size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr);
    CloseHandle(f);
    data.resize(got);
    if (data.size() >= 2 && data[0] == '\xff' && data[1] == '\xfe') {  // UTF-16 LE
        std::wstring wide(reinterpret_cast<const wchar_t*>(data.data() + 2), (data.size() - 2) / 2);
        return ToUtf8(wide);
    }
    return data;
}

}  // namespace

const char* HashKindName(HashKind k) {
    switch (k) {
        case HashKind::IbComponent: return "ib-component";
        case HashKind::Vb0Component: return "vb0-component";
        case HashKind::ShapeKey: return "shapekey";
        case HashKind::ConstantBuffer: return "constant-buffer";
        case HashKind::Texture: return "texture";
        default: return "other";
    }
}

void ModIndex::ParseFile(const std::wstring& path, const std::string& relative) {
    ++iniCount_;
    std::istringstream in(ReadText(path));
    std::string line, section;
    std::optional<ModEntry> cur;
    const auto flush = [&] {
        if (cur) {
            cur->kind = Classify(cur->section, cur->matchFirstIndex.has_value() || cur->matchIndexCount.has_value());
            sorted_[cur->hash].push_back(*cur);
        }
        cur.reset();
    };
    ModEntry pending;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') {
            flush();
            pending = ModEntry{};
            size_t end = line.find(']');
            section = end == std::string::npos ? line.substr(1) : line.substr(1, end - 1);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Lower(Trim(line.substr(0, eq)));
        const std::string value = Trim(line.substr(eq + 1));
        if (key == "hash") {
            try {
                pending.hash = static_cast<uint32_t>(std::stoul(value, nullptr, 16));
            } catch (...) {
                continue;
            }
            pending.file = relative;
            pending.section = section;
            cur = pending;
        } else if (key == "match_first_index") {
            pending.matchFirstIndex = ParseUint(value);
            if (cur) cur->matchFirstIndex = pending.matchFirstIndex;
        } else if (key == "match_index_count") {
            pending.matchIndexCount = ParseUint(value);
            if (cur) cur->matchIndexCount = pending.matchIndexCount;
        }
    }
    flush();
}

void ModIndex::Walk(const std::wstring& dir, const std::string& relative) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (StartsWithI(name, L"DISABLED") || _wcsicmp(name.c_str(), L"desktop.ini") == 0) continue;
        const std::string rel = relative.empty() ? ToUtf8(name) : relative + "/" + ToUtf8(name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            Walk(dir + L"\\" + name, rel);
        } else if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".ini") == 0) {
            ParseFile(dir + L"\\" + name, rel);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void ModIndex::Load(const std::wstring& modsDir) {
    dir_ = modsDir;
    Walk(modsDir, "");
    for (auto& [hash, entries] : sorted_) {
        // A hash can be used by several sections; reports label it by the first,
        // and the component is the meaningful one. HashKind is ordered accordingly.
        std::stable_sort(entries.begin(), entries.end(),
                         [](const ModEntry& a, const ModEntry& b) { return a.kind < b.kind; });
        byHash_[hash] = &entries;
        for (const ModEntry& e : entries) {
            if (e.kind == HashKind::IbComponent && e.matchIndexCount) indexCounts_.insert(*e.matchIndexCount);
        }
    }
}

const std::vector<ModEntry>& ModIndex::Entries(uint32_t hash) const {
    static const std::vector<ModEntry> empty;
    auto it = byHash_.find(hash);
    return it == byHash_.end() ? empty : *it->second;
}

// EFMIVULKAN_MODS, else "mods" in config.ini next to the DLL (the starter asks for it once).
std::wstring DefaultModsDir() {
    const std::wstring env = settings::Env(L"EFMIVULKAN_MODS");
    return env.empty() ? settings::Get(L"mods") : env;
}

}  // namespace efmivulkan
