// Auto-fix's analysis and ini rewriting, without the game.
#include <cstdio>
#include <string>

#include "autofix.h"

using namespace efmivulkan::autofix;

static int g_failed = 0;
static void Check(bool ok, const char* what) {
    printf("%-72s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failed;
}

static CapturedDraw Draw(uint32_t hash, uint32_t count, uint32_t colour, float x) {
    CapturedDraw d;
    d.hash = hash;
    d.indexCount = count;
    d.colorTargets = colour;
    d.hasPosition = true;
    d.position[0] = x;
    return d;
}

static BoundImage Image(uint32_t w, uint32_t h, uint32_t fmt, uint32_t measured, uint32_t modHash, uint32_t binding) {
    BoundImage b;
    b.width = w;
    b.height = h;
    b.vkFormat = fmt;
    b.measured = measured;
    b.modHash = modHash;
    b.at = {2, binding, 0, 0xabcdef};
    return b;
}

int main() {
    // ---- RewriteIni.
    {
        const std::string ini =
            "[TextureOverride_Component0]\r\nhash = 11111111\r\nmatch_index_count = 100\r\n"
            "[TextureOverride_Other]\r\nhash = 11111111\r\n"
            "[TextureOverride_Texture0]\r\nhash = AAAAAAAA\r\nthis = Resource_Texture0\r\n";
        size_t changed = 0;
        const std::string out = RewriteIni(ini, {{"textureoverride_component0", 0x11111111, 0x22222222, ""}}, {{"", 0xaaaaaaaa, 0xbbbbbbbb, ""}}, &changed);
        Check(changed == 2, "rewrite: two lines changed");
        Check(out.find("[TextureOverride_Component0]\r\nhash = 22222222\r\n") != std::string::npos, "rewrite: the component's own hash");
        Check(out.find("[TextureOverride_Other]\r\nhash = 11111111\r\n") != std::string::npos, "rewrite: the same hash in another section kept");
        Check(out.find("hash = bbbbbbbb\r\nthis") != std::string::npos, "rewrite: the texture hash");
        Check(out.size() == ini.size(), "rewrite: nothing else changed");
    }

    // ---- ComponentIdsOfName.
    {
        const auto ids = ComponentIdsOfName("Textures/Components-1-9 t=5720cd8a.dds");
        Check(ids.size() == 2 && ids[0] == 1 && ids[1] == 9, "texture file name: components 1 and 9");
        Check(ComponentIdsOfName("Textures/Logo.dds").empty(), "texture file name: none");
    }

    // ---- Analyze: a character whose meshes and one texture changed with an update.
    {
        ModInfo mod;
        mod.name = "Test";
        mod.components = {
            {0x01, 100, "TextureOverride_Component0", 0, 0, {}},
            {0x02, 50, "TextureOverride_Component1", 1, 0, {{17, 1}}},
            {0x03, 50, "TextureOverride_Component1_LOD1", 1, 1, {}},
            {0x04, 999, "TextureOverride_Component2", 2, 0, {}},  // Re-made by the update: no such count.
        };
        ModTexture diffuse;
        diffuse.file = "Components-0 t=0000aaaa.dds";
        diffuse.fileHash = 0xaaaa;
        diffuse.iniHashes = {0xaaaa};
        diffuse.width = diffuse.height = 64;
        diffuse.vkFormat = 146;  // BC7 sRGB; the game's is UNORM.
        diffuse.componentIds = {0};
        ModTexture own;  // Still matches: says where t17 is.
        own.file = "Components-1 t=0000cccc.dds";
        own.fileHash = 0xcccc;
        own.iniHashes = {0xcccc};
        own.width = own.height = 32;
        own.vkFormat = 145;
        own.componentIds = {1};
        own.everMatched = true;
        mod.textures = {diffuse, own};

        std::vector<CapturedDraw> draws;
        draws.push_back(Draw(0x10, 100, 2, 0.0f));  // Component 0, now.
        draws.back().images = {Image(64, 64, 145, 0xbbbb, 0, 3), Image(128, 128, 145, 0xdddd, 0, 4)};
        draws.push_back(Draw(0x10, 100, 0, 0.0f));  // Its shadow.
        draws.push_back(Draw(0x20, 50, 2, 0.1f));   // Component 1, LoD 0, in colour passes.
        draws.back().images = {Image(32, 32, 145, 0xcccc, 0xcccc, 5)};
        draws.push_back(Draw(0x30, 50, 0, 0.1f));   // Component 1, LoD 1: shadows only.
        draws.push_back(Draw(0x40, 100, 2, 9.0f));  // Another character, same count: not taken.

        const Analysis a = Analyze(draws, {mod}, {0x01, 0x02, 0x03, 0x04}, {0xaaaa, 0xcccc});
        Check(a.mods.size() == 1 && a.mods[0].onScreen, "analyze: the character found");
        const ModResult& r = a.mods[0];
        const auto remapped = [&](uint32_t from, uint32_t to) {
            for (const Remap& m : r.ib) {
                if (m.from == from && m.to == to) return true;
            }
            return false;
        };
        Check(remapped(0x01, 0x10), "analyze: component 0 to the same count on the same character");
        Check(remapped(0x02, 0x20), "analyze: LoD 0 to the draw in colour passes");
        Check(remapped(0x03, 0x30), "analyze: LoD 1 to the shadow-only draw");
        Check(r.ib.size() == 3, "analyze: the changed mesh left alone");
        Check(r.notes.size() == 1 && r.notes[0].find("Component2") != std::string::npos, "analyze: and reported");
        Check(r.textures.size() == 1 && r.textures[0].from == 0xaaaa && r.textures[0].to == 0xbbbb, "analyze: texture to the one of its size and format");
        const SlotLocation t17{2, 5, 0, 0xabcdef};
        Check(a.votes.count(17) && a.votes.at(17).count(t17) && a.votes.at(17).at(t17) == 1, "analyze: one vote for t17's place");

        // Up close: LoD 0 matches, LoD 1 is not drawn. A lone draw of LoD 1's size is not it.
        {
            std::vector<CapturedDraw> close;
            close.push_back(Draw(0x01, 100, 2, 0.0f));
            close.push_back(Draw(0x02, 50, 2, 0.0f));
            close.push_back(Draw(0x55, 50, 2, 0.0f));  // Something else of 50 indices.
            ModInfo m = mod;
            m.components.pop_back();
            const Analysis c = Analyze(close, {m}, {0x01, 0x02, 0x03}, {});
            bool lodNote = false;
            for (const std::string& n : c.mods[0].notes) lodNote |= n.find("LOD1") != std::string::npos;
            Check(c.mods.size() == 1 && c.mods[0].ib.empty() && !lodNote, "analyze: LoD 1 not drawn up close: left alone, not reported");
        }

        // Not on screen: nothing changed.
        const Analysis none = Analyze({Draw(0x99, 7, 2, 0.0f)}, {mod}, {0x01, 0x02, 0x03, 0x04}, {});
        Check(none.mods.size() == 1 && !none.mods[0].onScreen && none.mods[0].ib.empty(), "analyze: off screen, nothing remapped");
    }

    // ---- Several captures: two characters swapped in at the same place, fixed together.
    {
        ModInfo x, y;
        x.name = "X";
        x.components = {{0x01, 100, "TextureOverride_Component0", 0, 0, {}}, {0x02, 50, "TextureOverride_Component1", 1, 0, {}}};
        y.name = "Y";
        y.components = {{0x03, 70, "TextureOverride_Component0", 0, 0, {}}, {0x04, 30, "TextureOverride_Component1", 1, 0, {}}};
        std::vector<CapturedDraw> draws = {Draw(0x10, 100, 2, 0.0f), Draw(0x20, 50, 2, 0.0f),  // Capture 0: X's character.
                                           Draw(0x30, 70, 2, 0.0f), Draw(0x40, 30, 2, 0.0f), Draw(0x50, 100, 2, 0.0f)};  // Capture 1: Y's, there too.
        for (size_t i = 2; i < draws.size(); ++i) draws[i].capture = 1;
        const Analysis a = Analyze(draws, {x, y}, {0x01, 0x02, 0x03, 0x04}, {});
        Check(a.mods.size() == 2 && a.mods[0].ib.size() == 2 && a.mods[1].ib.size() == 2, "captures: both mods fixed from their own capture");
        bool tookY = false;
        for (const Remap& m : a.mods[0].ib) tookY |= m.to == 0x50;
        Check(!tookY, "captures: the same place in another capture is another character");
    }

    // ---- Slots: votes, the decision, the file.
    {
        SlotVotes v;
        const SlotLocation a{2, 5, 0, 1}, b{2, 6, 0, 1};
        v[17][a] = 1;
        Check(Decide(v).empty(), "slots: one vote is not enough");
        v[17][a] = 3;
        v[17][b] = 1;
        Check(Decide(v).count(17) && Decide(v).at(17) == std::vector<SlotLocation>{a}, "slots: 3 of 4 votes decide");
        v[17][b] = 2;
        Check(!Decide(v).count(17), "slots: 3 of 5 do not");
        {
            // Another shader's layout: register 17 is another binding there, decided on its own.
            SlotVotes w;
            const SlotLocation other{2, 9, 0, 2};
            w[17][a] = 3;
            w[17][other] = 2;
            Check(Decide(w).count(17) && Decide(w).at(17).size() == 2, "slots: a place per set layout");
            // A binding two registers claim is where a texture happens to be: neither gets it.
            w[18][a] = 2;
            const Learned d = Decide(w);
            Check(d.count(17) && d.at(17) == std::vector<SlotLocation>{other} && !d.count(18), "slots: a binding two registers claim is nobody's");
        }
        const SlotVotes back = ParseSlots(FormatSlots(v));
        Check(back == v, "slots: the file reads back the same");
        SlotVotes more;
        more[17][a] = 2;
        AddVotes(&v, more);
        Check(v[17][a] == 5, "slots: captures add up");
    }

    printf(g_failed ? "%d FAILED\n" : "all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
