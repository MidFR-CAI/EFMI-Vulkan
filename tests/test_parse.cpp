// Parses every EFMI mod in a Mods folder and prints, per component, what milestone 2
// would draw: buffers, draw calls, textures, and whether it needs the merged skeleton.
//
//   test_parse <Mods folder>
#include <windows.h>

#include <cstdio>
#include <string>

#include "efmimod.h"
#include "log.h"

using namespace efmivulkan;

static int g_mods = 0, g_components = 0, g_empty = 0;

static void Visit(const std::wstring& dir, const std::string& relative, const std::wstring& userIni) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || _wcsnicmp(name.c_str(), L"DISABLED", 8) == 0) continue;
        const std::string rel = relative.empty() ? ToUtf8(name) : relative + "/" + ToUtf8(name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            Visit(dir + L"\\" + name, rel, userIni);
            continue;
        }
        if (name.size() < 4 || _wcsicmp(name.c_str() + name.size() - 4, L".ini") != 0) continue;
        const auto mod = ParseEfmiMod(dir + L"\\" + name, rel, userIni);
        if (!mod) continue;
        ++g_mods;
        printf("%s: identification=%d min=%d scale=%g, %zu components, %zu buffers, %zu textures, %zu keys\n", mod->name.c_str(), int(mod->spatialIdentification), mod->minComponents, mod->meshScale, mod->components.size(), mod->buffers.size(),
               mod->textures.size(), mod->keys.size());
        for (const auto& c : mod->components) {
            ++g_components;
            if (c.list.draws.empty()) ++g_empty;
            uint32_t indices = 0;
            for (const auto& d : c.list.draws) indices += d.indexCount;
            printf("  %08x count=%-6u lod=%d id=%-2d %s ib=%s draws=%zu (%u indices) vb=", c.hash, c.indexCount, c.lod, c.componentId,
                   c.gpuPosed ? "gpu_posed" : "own bones", c.list.ib >= 0 ? mod->buffers[c.list.ib].name.c_str() : "-",
                   c.list.draws.size(), indices);
            for (int s = 0; s < kEfmiVbSlots; ++s) {
                if (c.list.vb[s] >= 0) printf("%d:%u ", s, mod->buffers[c.list.vb[s]].stride);
            }
            size_t cross = 0;
            size_t filtered = 0;
            for (const auto& d : c.list.draws) cross += d.boneSource >= 0, filtered += d.shaderFiltered;
            printf("filtered=%zu tex=%zu records=%zu cross=%zu left-out=%u original=%d\n", filtered, c.list.textures.size(), c.list.recordIds.size(), cross, c.list.otherBonesDraws, int(c.list.drawOriginal));
        }
        for (const auto& k : mod->keys) printf("  key vk=%d %s (%zu values)\n", k.vk, k.var.c_str(), k.values.size());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        printf("usage: test_parse <Mods folder>\n");
        return 2;
    }
    const std::wstring mods = argv[1];
    Visit(mods, "", mods.substr(0, mods.find_last_of(L'\\')) + L"\\d3dx_user.ini");
    printf("%d mods, %d components, %d with nothing to draw\n", g_mods, g_components, g_empty);
    return 0;
}
