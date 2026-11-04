// The 3DMigoto ini language, as much of it as mods use for toggles. Shared by every
// part that reads a mod's ini.
#include "ini.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "log.h"

namespace efmivulkan::ini {

constexpr int kMaxRunDepth = 4;


std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::vector<std::string> Split(const std::string& s, const std::string& by) {
    std::vector<std::string> out;
    size_t at = 0, next;
    while ((next = s.find(by, at)) != std::string::npos) {
        out.push_back(s.substr(at, next - at));
        at = next + by.size();
    }
    out.push_back(s.substr(at));
    return out;
}

bool KeyValue(const std::string& line, std::string* key, std::string* value) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    *key = Lower(Trim(line.substr(0, eq)));
    *value = Trim(line.substr(eq + 1));
    return true;
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::vector<Section> ReadSections(const std::wstring& path) {
    std::vector<Section> out;
    std::ifstream f(path, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') {
            size_t end = line.find(']');
            out.push_back({Lower(end == std::string::npos ? line.substr(1) : line.substr(1, end - 1)), {}});
        } else if (!out.empty()) {
            out.back().lines.push_back(line);
        }
    }
    return out;
}

const Section* FindSection(const std::vector<Section>& sections, const std::string& lowerName) {
    for (const Section& s : sections) {
        if (s.name == lowerName) return &s;
    }
    return nullptr;
}

// Only toggles decide anything here. Terms about other state, such as $mod_enabled
// or "ResourceX !== null", describe WWMI's own bookkeeping and count as true.


// 3DMigoto expressions: numbers, $variables, ( ), ! and unary -, * /, + -, comparisons,
// && and ||. A value is unknown when it depends on anything else (resources, shader
// slots, WWMI's namespaced variables), and unknown spreads, except where && and ||
// are decided without it.
class Evaluator {
public:
    Evaluator(const std::string& text, const Vars& vars) : s_(text), vars_(vars) {}

    std::optional<double> Run() {
        auto v = Or();
        Skip();
        return i_ == s_.size() ? v : std::nullopt;
    }

private:
    using Value = std::optional<double>;

    void Skip() {
        while (i_ < s_.size() && isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    bool Eat(const char* op) {
        Skip();
        const size_t n = std::strlen(op);
        if (s_.compare(i_, n, op) != 0) return false;
        i_ += n;
        return true;
    }
    static Value Bool(bool b) { return b ? 1.0 : 0.0; }

    Value Or() {
        Value left = And();
        while (Eat("||")) {
            const Value right = And();
            if ((left && *left != 0) || (right && *right != 0)) {
                left = 1.0;
            } else {
                left = left && right ? Value(0.0) : std::nullopt;
            }
        }
        return left;
    }
    Value And() {
        Value left = Compare();
        while (Eat("&&")) {
            const Value right = Compare();
            if ((left && *left == 0) || (right && *right == 0)) {
                left = 0.0;
            } else {
                left = left && right ? Value(1.0) : std::nullopt;
            }
        }
        return left;
    }
    Value Compare() {
        Value left = Add();
        for (;;) {
            if (Eat("===") || Eat("!==")) {  // Resource identity: never known here.
                Add();
                left = std::nullopt;
            } else if (Eat("==")) {
                const Value r = Add();
                left = left && r ? Bool(*left == *r) : std::nullopt;
            } else if (Eat("!=")) {
                const Value r = Add();
                left = left && r ? Bool(*left != *r) : std::nullopt;
            } else if (Eat("<=")) {
                const Value r = Add();
                left = left && r ? Bool(*left <= *r) : std::nullopt;
            } else if (Eat(">=")) {
                const Value r = Add();
                left = left && r ? Bool(*left >= *r) : std::nullopt;
            } else if (Eat("<")) {
                const Value r = Add();
                left = left && r ? Bool(*left < *r) : std::nullopt;
            } else if (Eat(">")) {
                const Value r = Add();
                left = left && r ? Bool(*left > *r) : std::nullopt;
            } else {
                return left;
            }
        }
    }
    Value Add() {
        Value left = Multiply();
        for (;;) {
            if (Eat("+")) {
                const Value r = Multiply();
                left = left && r ? Value(*left + *r) : std::nullopt;
            } else if (Eat("-")) {
                const Value r = Multiply();
                left = left && r ? Value(*left - *r) : std::nullopt;
            } else {
                return left;
            }
        }
    }
    Value Multiply() {
        Value left = Unary();
        for (;;) {
            if (Eat("*")) {
                const Value r = Unary();
                left = left && r ? Value(*left * *r) : std::nullopt;
            } else if (Eat("/")) {
                const Value r = Unary();
                left = left && r && *r != 0 ? Value(*left / *r) : std::nullopt;
            } else {
                return left;
            }
        }
    }
    Value Unary() {
        if (Eat("!")) {
            const Value v = Unary();
            return v ? Bool(*v == 0) : std::nullopt;
        }
        if (Eat("-")) {
            const Value v = Unary();
            return v ? Value(-*v) : std::nullopt;
        }
        return Primary();
    }
    Value Primary() {
        Skip();
        if (i_ >= s_.size()) return std::nullopt;
        const char c = s_[i_];
        if (c == '(') {
            ++i_;
            const Value v = Or();
            Eat(")");
            return v;
        }
        if (isdigit(static_cast<unsigned char>(c)) || c == '.') {
            char* end = nullptr;
            const double v = std::strtod(s_.c_str() + i_, &end);
            i_ = static_cast<size_t>(end - s_.c_str());
            return v;
        }
        // $name, $\namespace\name, ResourceX, ps-t0, null, time...
        const size_t start = i_;
        while (i_ < s_.size() && (isalnum(static_cast<unsigned char>(s_[i_])) || strchr("$_\\.", s_[i_]) ||
                                  (s_[i_] == '-' && i_ > start && s_[start] != '$'))) {
            ++i_;
            // EFMI names variables like $draw_component_8_cpu_posed_(only_textures_modding_supported):
            // a "(...)" right after a variable's name is part of it.
            if (s_[start] == '$' && i_ < s_.size() && s_[i_] == '(' && s_[i_ - 1] == '_') {
                const size_t close = s_.find(')', i_);
                if (close != std::string::npos && s_.find_first_of(" \t", i_) > close) i_ = close + 1;
            }
        }
        if (i_ == start) {
            ++i_;  // Not an expression; consume it so parsing ends.
            return std::nullopt;
        }
        const std::string name = Lower(s_.substr(start, i_ - start));
        if (name[0] != '$' || name.find('\\') != std::string::npos) return std::nullopt;
        const auto it = vars_.find(name);
        return it == vars_.end() ? std::nullopt : Value(it->second);
    }

    const std::string& s_;
    const Vars& vars_;
    size_t i_ = 0;
};

std::optional<double> Evaluate(const std::string& expression, const Vars& vars) { return Evaluator(expression, vars).Run(); }

// An unknown condition counts as true: what it depends on is WWMI's own state,
// which is true for a mod that is running.
bool EvalCondition(const std::string& condition, const Vars& vars) {
    const auto v = Evaluate(condition, vars);
    return !v || *v != 0;
}

// "$x = expression" for the mod's own variables. Unknown values leave x unchanged.
bool Assign(const std::string& key, const std::string& value, Vars& vars) {
    if (key.empty() || key[0] != '$' || key.find('\\') != std::string::npos) return false;
    if (const auto v = Evaluate(Lower(value), vars)) vars[key] = static_cast<int>(std::lround(*v));
    return true;
}

Vars ReadToggles(const std::vector<Section>& sections, const std::string& relative, const std::wstring& userIni,
                 const std::map<std::string, int>* toggles) {
    Vars vars;
    for (const Section& s : sections) {
        if (s.name.rfind("key", 0) != 0) continue;
        for (const std::string& line : s.lines) {
            std::string key, value;
            if (KeyValue(line, &key, &value) && key[0] == '$') vars[key] = 0;
        }
    }
    // Other globals keep their declared value, so "if $body == 1" takes its else
    // branch until something sets it. WWMI's bookkeeping stays unknown: those are all
    // true once a mod is running.
    static const char* const kBookkeeping[] = {"$mod_enabled", "$object_detected", "$merge_status", "$mod_id", "$active",
                                               "$state_id", "$required_wwmi", "$object_guid", "$mesh_vertex", "$shapekey_vertex"};
    if (const Section* constants = FindSection(sections, "constants")) {
        for (const std::string& raw : constants->lines) {
            std::string line = Lower(raw);
            size_t at = line.find('$');
            if (line.rfind("global", 0) != 0 || at == std::string::npos) continue;
            size_t eq = line.find('=', at);
            const std::string name = Trim(line.substr(at, eq == std::string::npos ? std::string::npos : eq - at));
            bool bookkeeping = false;
            for (const char* prefix : kBookkeeping) bookkeeping = bookkeeping || name.rfind(prefix, 0) == 0;
            if (bookkeeping || vars.count(name)) continue;
            vars[name] = eq == std::string::npos ? 0 : std::atoi(line.c_str() + eq + 1);
        }
    }

    // 3DMigoto stores these as $\mods\<lower case path>\<var> = value.
    std::string prefix = "$\\mods\\" + Lower(relative) + "\\";
    std::replace(prefix.begin(), prefix.end(), '/', '\\');
    std::ifstream f(userIni, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        std::string key, value;
        if (!KeyValue(Trim(line), &key, &value) || key.rfind(prefix, 0) != 0) continue;
        auto it = vars.find("$" + key.substr(prefix.size()));
        if (it == vars.end()) continue;
        try {
            it->second = static_cast<int>(std::stof(value));
        } catch (...) {
        }
    }
    if (toggles) {
        for (const auto& [name, value] : *toggles) {
            if (vars.count(name)) vars[name] = value;
        }
    }
    return vars;
}

// 3DMigoto key names: VK_ constants, or a single character.
int VirtualKey(const std::string& lowerName) {
    static const std::map<std::string, int> kNames = {
        {"vk_left", VK_LEFT}, {"vk_right", VK_RIGHT}, {"vk_up", VK_UP}, {"vk_down", VK_DOWN},
        {"vk_home", VK_HOME}, {"vk_end", VK_END}, {"vk_insert", VK_INSERT}, {"vk_delete", VK_DELETE},
        {"vk_prior", VK_PRIOR}, {"vk_next", VK_NEXT}, {"vk_space", VK_SPACE}, {"vk_tab", VK_TAB},
        {"vk_multiply", VK_MULTIPLY}, {"vk_add", VK_ADD}, {"vk_subtract", VK_SUBTRACT}, {"vk_divide", VK_DIVIDE},
        {"vk_decimal", VK_DECIMAL}};
    auto it = kNames.find(lowerName);
    if (it == kNames.end()) it = kNames.find("vk_" + lowerName);  // 3DMigoto takes both "VK_DOWN" and "DOWN".
    if (it != kNames.end()) return it->second;
    if (lowerName.rfind("vk_numpad", 0) == 0 && lowerName.size() == 10 && isdigit(static_cast<unsigned char>(lowerName[9]))) {
        return VK_NUMPAD0 + (lowerName[9] - '0');
    }
    const std::string f = lowerName.rfind("vk_", 0) == 0 ? lowerName.substr(3) : lowerName;
    if (f.size() >= 2 && f[0] == 'f' && isdigit(static_cast<unsigned char>(f[1]))) {
        const int n = std::atoi(f.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (lowerName.size() == 1) {
        const SHORT scan = VkKeyScanA(lowerName[0]);
        if (scan != -1) return scan & 0xff;
    }
    return 0;
}

std::vector<ModKey> ParseKeys(const std::vector<Section>& sections, const std::string& relative) {
    std::vector<ModKey> keys;
    for (const Section& s : sections) {
        if (s.name.rfind("key", 0) != 0) continue;
        ModKey key;
        std::string type = "cycle";
        for (const std::string& line : s.lines) {
            std::string k, value;
            if (!KeyValue(line, &k, &value)) continue;
            if (k == "key") {
                // Modifiers come first; the key itself is the last word.
                const std::string low = Lower(value);
                key.vk = VirtualKey(low.substr(low.find_last_of(" \t") == std::string::npos ? 0 : low.find_last_of(" \t") + 1));
            } else if (k == "type") {
                type = Lower(value);
            } else if (k == "condition") {
                const std::string low = Lower(value);
                key.whenVisible = low.find("$active") != std::string::npos || low.find("$object_detected") != std::string::npos;
            } else if (k[0] == '$') {
                key.var = k;
                for (const std::string& part : Split(value, ",")) key.values.push_back(std::atoi(Trim(part).c_str()));
            }
        }
        if (type != "cycle" || !key.vk || key.var.empty() || key.values.empty()) {
            LogError("%s: [%s] not supported (type %s)", relative.c_str(), s.name.c_str(), type.c_str());
            continue;
        }
        keys.push_back(std::move(key));
    }
    return keys;
}

// What [Present] does each frame to the mod's own variables, such as deriving
// "$draw_component_3 = ($swapvar_booba == 0)" from its toggles.
void RunPresent(const std::vector<Section>& sections, Vars& vars) {
    std::function<void(const Section&, int)> run = [&](const Section& section, int depth) {
        ForActiveLines(section, vars, [&](const std::string& line) {
            std::string key, value;
            if (!KeyValue(line, &key, &value) || Assign(key, value, vars)) return;
            if (key == "run" && depth < kMaxRunDepth) {
                if (const Section* list = FindSection(sections, Lower(value))) run(*list, depth + 1);
            }
        });
    };
    for (const Section& s : sections) {
        if (s.name == "present") run(s, 0);
    }
}

}  // namespace efmivulkan::ini
