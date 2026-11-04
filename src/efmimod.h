// What one EFMI mod draws, read from its ini. A component is a TextureOverride with a
// hash (an index buffer region) and match_index_count; it runs a draw list:
//
//   [TextureOverride_Component0]                      (older mods)
//   hash = d1d451ba
//   match_index_count = 74298
//   $lod_level = 0
//   run = CommandList_Draw_Component0
//
//   [TextureOverride_EntryPoint_Component0]           (newer mods)
//   $\EFMIv1\gpu_posed = 1
//   CommandList\EFMIv1\Callback_Component_DrawCustom = ref CommandList_Draw_Component0
//
//   [CommandList_Draw_Component0]
//   ib = ref Resource_Component0_IB
//   if $lod_level == 0
//       vb0 = ref Resource_Component0_VB0  ...
//   ps-t16 = Resource_Texture1
//   if $draw_component_0_x
//       drawindexed = 37980, 0, 0
//   drawindexedinstanced = 14934, INSTANCE_COUNT, 0, 0, FIRST_INSTANCE
//
// Toggles change which lines are active, so the draw lists are worked out again, with
// the new values, when a key is pressed. Buffer indices depend only on the ini.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ini.h"

namespace efmivulkan {

constexpr int kEfmiVbSlots = 8;

struct EfmiBuffer {
    std::string name;  // Lower case, the Resource section.
    std::wstring path;
    uint32_t stride = 0;
    uint32_t indexBytes = 0;  // For index buffers: 2 (R16_UINT) or 4 (R32_UINT).
    std::string format;       // Lower case, as the ini gives it.
};

struct EfmiDrawCall {
    uint32_t indexCount = 0, firstIndex = 0;
    int32_t baseVertex = 0;
    // The buffers bound when it was issued: a list can switch them between draws.
    int ib = -1;
    std::array<int, 8> vb{-1, -1, -1, -1, -1, -1, -1, -1};
    // Bones recorded by another component (EfmiMod::boneRecordIds), -1: the draw's own.
    int boneSource = -1;
    // Under "if vs == N": in DX11 only in some passes. The layer cannot tell which shaders
    // those are; it leaves these draws out of outline passes (efmidraw).
    bool shaderFiltered = false;
};

struct EfmiTextureBind {
    int slot;     // ps-tN
    int texture;  // Index into EfmiMod::textures.
};

// What a component draws, with the toggles as they are.
struct EfmiDrawList {
    int ib = -1;  // Index into EfmiMod::buffers.
    std::array<int, kEfmiVbSlots> vb{-1, -1, -1, -1, -1, -1, -1, -1};
    std::vector<EfmiDrawCall> draws;  // Empty: the component is hidden.
    // Draws left out: they come after the list binds bones of its own (vs-t0, vs-cbN) the
    // layer cannot tell the source of.
    uint32_t otherBonesDraws = 0;
    // Bone records this list makes of the draw's own bones, for other components' draws.
    std::vector<int> recordIds;
    // "draw = from_caller" or "drawindexed = INDEX_COUNT, FIRST_INDEX, 0": the game's own
    // draw, with the mod's textures (its list runs OverrideTextures).
    bool drawOriginal = false;
    std::vector<EfmiTextureBind> textures;
    // RabbitFX's Diffuse, Lightmap, Normalmap: indices into EfmiMod::textures, -1 where none.
    int rabbit[3] = {-1, -1, -1};
};

struct EfmiComponent {
    uint32_t hash = 0;
    uint32_t indexCount = 0;
    std::string section;
    int componentId = -1;
    int lod = 0;
    bool gpuPosed = false;  // Its bone indices are EFMI's merged skeleton's.
    bool readOnly = false;  // skip_skeleton_override: its bones go into the merged skeleton, not the other way.
    EfmiDrawList list;
};

// A gpu_posed mod's merged skeleton, from its [CommandListInitializeMergedSkeleton]:
//   $cmp_id = 1
//   $Pool_MergedSkeleton_Component_VertexGroupOffsets[$cmp_id] = 116
//   $Pool_MergedSkeleton_Component_VertexGroupCounts[$cmp_id] = 58
//   Pool_MergedSkeleton_Component_LodRemaps[$cmp_id * $lod_level_count + 1] = ref Resource_X
struct EfmiSkeletonComponent {
    int offset = -1, count = 0;
    int lodRemap[4] = {-1, -1, -1, -1};  // Index into EfmiMod::buffers (R16_UINT), -1: none.
};

struct EfmiMod {
    std::string name;  // Below the Mods folder.
    std::wstring ini;
    std::vector<EfmiBuffer> buffers;
    std::vector<std::wstring> textures;
    // The game texture each one replaces: the hash in its file name ("... t=3156b9c5 ..."),
    // 0 when the name has none.
    std::vector<uint32_t> textureHashes;
    // TextureOverride sections without match_index_count: "hash = X" with "this = Resource".
    std::map<uint32_t, int> thisByHash;
    std::vector<EfmiComponent> components;
    // Cross-IB bones, as Mifu's mod does them with custom shaders: a "record" shader
    // (cs-t0 = vs-t0) copies a draw's bones under an ID (the cs-t2 resource), a later draw
    // binds them (vs-t0 = ...) with the same ID. The IDs, by resource name.
    std::vector<std::string> boneRecordIds;
    uint32_t bonesCount = 0;                              // $bones_count; 0: no merged skeleton.
    int minComponents = 0;  // $\EFMIv1\identification_min_components, 0 where the mod sets none.
    // run = CommandList\EFMIv1\SpatialIdentity_IdentifyComponentInstances: objects are
    // identified as EFMI's SpatialIdentification.ini does; else every draw is the mod's.
    bool spatialIdentification = false;
    float meshScale = 1.0f;  // $\EFMIv1\custom_mesh_scale.
    std::vector<EfmiSkeletonComponent> skeletonComponents;  // By component id.
    std::vector<ini::ModKey> keys;
    std::map<std::string, int> toggles;  // Current values of the variables keys change.
};

// ini: full path. relative: below the Mods folder. userIni: d3dx_user.ini. toggles: values
// that win over the ini's and the user ini's.
std::optional<EfmiMod> ParseEfmiMod(const std::wstring& ini, const std::string& relative, const std::wstring& userIni,
                                    const std::map<std::string, int>* toggles = nullptr);

}  // namespace efmivulkan
