// Auto-fix: mods a game update broke, fixed from what the game itself draws.
//
// Shift+F8 in game (or F in the EFMIVulkan window): mods go off for a few frames, one frame
// of the game's own draws is captured, with the textures each binds, and kept for the
// session; then every capture kept fixes, per mod (with debug keys on, Ctrl+F10 only captures
// and previews, Ctrl+Shift+F10 fixes):
//
//   meshes    A component whose index buffer hash the game no longer draws gets the hash of
//             the game's draw with the same index count, on the same character (draws are
//             grouped by the position in their bone config), that no mod claims. LoD 1
//             components prefer draws seen only in shadow passes, LoD 0 ones draws in passes
//             with colour targets. Only a single candidate is taken.
//   textures  A mod texture whose hash the game never showed gets the hash of the one game
//             texture of the same size and format, bound in its components' draws, that no
//             mod claims.
//   slots     DX11 texture registers (ps-tN) have no meaning on Vulkan. Where a mod's ps-tN
//             texture is found bound in its own component's draw, that is a vote for where
//             register tN is on Vulkan (set, binding, element, set layout). Votes add up in
//             slots.ini over captures; a clear winner lets ps-tN lines put their texture into
//             that slot of the game's draw (Zhuang's: one component's texture in another's
//             draw, which no hash replacement can do).
//
// Changed inis are backed up next to themselves ("<ini> <date>.BAK", which no loader reads)
// and rewritten; the report is autofix.log. The game is restarted to load them.
//
// This header's first half is pure (tested by test_autofix); the runtime is autofix.cpp.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace efmivulkan::autofix {

// ---- What a capture holds.

struct SlotLocation {
    uint32_t set = 0, binding = 0, element = 0;
    uint64_t layoutSig = 0;  // The set layout's shape (efmitex::LayoutSignature).
    bool operator<(const SlotLocation& o) const {
        if (set != o.set) return set < o.set;
        if (binding != o.binding) return binding < o.binding;
        if (element != o.element) return element < o.element;
        return layoutSig < o.layoutSig;
    }
    bool operator==(const SlotLocation& o) const {
        return set == o.set && binding == o.binding && element == o.element && layoutSig == o.layoutSig;
    }
};

struct BoundImage {
    SlotLocation at;
    uint64_t imageId = 0;
    uint32_t width = 0, height = 0, vkFormat = 0;
    uint32_t modHash = 0;   // The mod texture hash it matched, 0: none.
    uint32_t measured = 0;  // Its DX11 texture hash as measured, 0: its pixels never seen.
};

struct CapturedDraw {
    uint32_t capture = 0;  // Which capture: draws of different captures are never one character.
    uint32_t hash = 0;     // Index region hash, 0: data never seen.
    uint32_t indexCount = 0, firstIndex = 0;
    uint32_t colorTargets = 0;  // 0: a depth or shadow pass.
    bool hasPosition = false;
    float position[3] = {};
    // For finding where Vulkan's instance config keeps an object's place: all 16 rows, and the
    // translation of the draw's first bone (bone 0's matrix rows' w).
    bool hasConfig = false;
    float config[64] = {};
    bool hasBone0 = false;
    float bone0[3] = {};
    // How the draw picks its entry of the 64 KB config block: its push constants, and every
    // entry of the block that is a valid instance config (index, position, row 5's bone row).
    std::vector<uint8_t> pushed;
    struct ConfigEntry {
        uint32_t index;
        float position[3];
        uint32_t boneRow;
    };
    std::vector<ConfigEntry> entries;
    uint32_t configOffset = 0;  // The block's offset in its buffer (dynamic offset included).
    std::vector<BoundImage> images;
};

// ---- What a mod holds, for the analysis.

struct ModTexture {
    std::string file;               // UTF-8, for the report.
    uint32_t fileHash = 0;          // "t=..." in its file name.
    std::vector<uint32_t> iniHashes;  // TextureOverride hashes that put it in.
    uint32_t width = 0, height = 0, vkFormat = 0;  // From its DDS, 0: unreadable.
    std::vector<int> componentIds;  // "Components-1-9" in its file name; empty: unknown.
    bool everMatched = false;       // The game showed one of its hashes this session.
};

struct ModComponent {
    uint32_t hash = 0, indexCount = 0;
    std::string section;  // As in the ini (any case).
    int componentId = -1;
    int lod = 0;
    std::vector<std::pair<int, int>> slots;  // ps-tN: (N, index into ModInfo::textures).
};

struct ModInfo {
    std::string name;  // Below the Mods folder.
    std::wstring ini;
    std::vector<ModComponent> components;
    std::vector<ModTexture> textures;
};

// ---- The analysis.

struct Remap {
    std::string section;  // IB remaps: the component's section. Texture remaps: empty.
    uint32_t from = 0, to = 0;
    std::string why;
};

struct ModResult {
    size_t mod = 0;  // Index into the mods analysed.
    bool onScreen = false;
    std::vector<Remap> ib, textures;
    std::vector<std::string> notes;  // What could not be fixed, and why.
};

using SlotVotes = std::map<int, std::map<SlotLocation, uint32_t>>;  // ps-tN -> where -> votes.

struct Analysis {
    std::vector<ModResult> mods;
    SlotVotes votes;  // This capture's.
};

// claimedIb / claimedTextures: every hash any installed mod names (a hash another mod uses
// is never taken).
Analysis Analyze(const std::vector<CapturedDraw>& draws, const std::vector<ModInfo>& mods, const std::set<uint32_t>& claimedIb,
                 const std::set<uint32_t>& claimedTextures);

// The ini with the remaps applied to its "hash = " lines: IB remaps in their own section only,
// texture remaps in any section that is not an IB remap's. *changed: lines changed.
std::string RewriteIni(const std::string& text, const std::vector<Remap>& ib, const std::vector<Remap>& textures, size_t* changed);

// slots.ini: one line per slot and place, "t17 = <set> <binding> <element> <layout> <votes>".
SlotVotes ParseSlots(const std::string& text);
std::string FormatSlots(const SlotVotes& votes);
void AddVotes(SlotVotes* into, const SlotVotes& more);
// Each slot's places, one per set layout: register N is another binding in another shader's
// layout. A layout's place is taken where it has at least 2 votes and 3 in 4 of the slot's
// votes in that layout.
using Learned = std::map<int, std::vector<SlotLocation>>;
Learned Decide(const SlotVotes& votes);

// The component ids in an EFMI texture file name: "Components-1-9 t=..." -> {1, 9}.
std::vector<int> ComponentIdsOfName(const std::string& fileName);

// ---- Runtime (autofix.cpp).

struct SlotReplace {
    SlotLocation at;
    int texture = -1;  // efmitex id.
};

// Ctrl+F10 (F in the launcher): one capture, kept with the others of this session; the
// report previews what all of them would fix. Ctrl+Shift+F10 (A in the launcher): the kept
// captures analysed together, every mod they can fix fixed, and the captures let go.
void RequestCapture();
void RequestApply();
// Shift+F8 (F in the launcher): a capture, then a fix from every capture kept this session.
void RequestFix();
bool ModsOff();          // Mods held off for the capture.
bool Recording();        // This frame's draws are wanted.
void OnDraw(CapturedDraw&& draw);
void OnPresent();
// The learned slots (Decide over slots.ini), loaded at start and after each capture.
std::shared_ptr<const Learned> LearnedSlots();
void LoadLearnedSlots();

}  // namespace efmivulkan::autofix
