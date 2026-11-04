// The 3DMigoto ini language, as much of it as mods use for toggles: sections,
// expressions ($vars, comparisons, && || !, arithmetic), if/elif/else blocks,
// "$var = expr", [Present] and the command lists it runs, and [Key*] sections.
#pragma once
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace efmivulkan::ini {

struct Section {
    std::string name;  // Lower case.
    std::vector<std::string> lines;
};
using Vars = std::map<std::string, int>;

// A [Key*] section. Mods use type = cycle.
struct ModKey {
    int vk = 0;
    std::string var;           // Lower case, with the $.
    std::vector<int> values;   // Cycled through in order.
    bool whenVisible = false;  // condition = $active == 1 or $object_detected == 1.
};

std::string Lower(std::string s);
std::string Trim(const std::string& s);
std::vector<std::string> Split(const std::string& s, const std::string& by);
bool KeyValue(const std::string& line, std::string* key, std::string* value);  // key in lower case.
std::wstring Widen(const std::string& s);

std::vector<Section> ReadSections(const std::wstring& path);
const Section* FindSection(const std::vector<Section>& sections, const std::string& lowerName);

// Unknown (unset variables, the engine's own state) is nullopt; a condition that is
// unknown counts as true, as it is for a mod that is running.
std::optional<double> Evaluate(const std::string& expression, const Vars& vars);
bool EvalCondition(const std::string& condition, const Vars& vars);
// "$x = expression": true when the line was an assignment to a mod variable.
bool Assign(const std::string& key, const std::string& value, Vars& vars);

// The toggle variables with their values: defaults, then d3dx_user.ini, then toggles.
Vars ReadToggles(const std::vector<Section>& sections, const std::string& relative, const std::wstring& userIni,
                 const std::map<std::string, int>* toggles);
std::vector<ModKey> ParseKeys(const std::vector<Section>& sections, const std::string& relative);
int VirtualKey(const std::string& lowerName);
void RunPresent(const std::vector<Section>& sections, Vars& vars);

// How many enclosing if blocks compare the shader ("if vs == 200 || vs == 201"): 3DMigoto's
// ShaderOverride filter_index, which the layer cannot evaluate (no DX11 shaders on Vulkan).
// Read from a ForActiveLines callback; nested walks add up.
inline thread_local int t_shaderFiltered = 0;
inline bool ComparesShader(const std::string& lowCondition) {
    for (const char* s : {"vs ==", "vs==", "ps ==", "ps==", "vs !=", "ps !="}) {
        const size_t at = lowCondition.find(s);
        if (at != std::string::npos && (at == 0 || !isalnum(static_cast<unsigned char>(lowCondition[at - 1])))) return true;
    }
    return false;
}

// Calls line(text) for each line of the section that its if/elif/else blocks leave
// active. Conditions use vars as they are when reached, as assignments may change them.
template <class F>
void ForActiveLines(const Section& section, const Vars& vars, F&& line) {
    struct Block {
        bool parent, taken, active;
        bool filtered;
    };
    std::vector<Block> stack;
    const auto active = [&] { return stack.empty() || stack.back().active; };
    for (const std::string& text : section.lines) {
        const std::string low = Lower(text);
        if (low.rfind("if ", 0) == 0) {
            const bool parent = active();
            const bool on = parent && EvalCondition(low.substr(3), vars);
            const bool filtered = ComparesShader(low.substr(3));
            t_shaderFiltered += filtered;
            stack.push_back({parent, on, on, filtered});
        } else if ((low.rfind("elif ", 0) == 0 || low.rfind("else if ", 0) == 0) && !stack.empty()) {
            Block& b = stack.back();
            const bool on = !b.taken && b.parent && EvalCondition(low.substr(low[1] == 'l' && low[2] == 'i' ? 5 : 8), vars);
            b.taken = b.taken || on;
            b.active = on;
        } else if (low == "else" && !stack.empty()) {
            Block& b = stack.back();
            b.active = b.parent && !b.taken;
            b.taken = true;
        } else if (low == "endif" && !stack.empty()) {
            t_shaderFiltered -= stack.back().filtered;
            stack.pop_back();
        } else if (active()) {
            line(text);
        }
    }
    for (const Block& b : stack) t_shaderFiltered -= b.filtered;  // Blocks left open.
}

}  // namespace efmivulkan::ini
