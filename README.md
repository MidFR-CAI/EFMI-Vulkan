# EFMIVulkan

EFMI (Arknights: Endfield Model Importer) mods on Vulkan. XXMI runs EFMI through 3DMigoto,
which needs the game on DX11. Endfield's other renderer, and the official launcher's default,
is Vulkan (it has no DX12 option). This is a Vulkan layer that does EFMI's work there.
Built from WWMI12, the Wuthering Waves DX12 version.

**Milestone 2** (now): mod meshes, their textures, and EFMI's merged skeleton. Mods whose
components use the game's own bones (Mifu, Endmin) and `gpu_posed` mods (merged skeleton,
such as 余烬) are drawn. Draws of several instances are still left to the game (2d).

Milestone 1, identification: the layer finds which of your mods' components the game draws:

- EFMI keys each component on an index buffer *region hash* (the XXMI fork's
  `track_region_hashes`): crc32c, seed 0, over exactly the index bytes a draw reads, from
  `offset + index size × firstIndex` for `index size × indexCount` bytes. Nothing of the
  buffer description goes in. `match_index_count` filters by the draw's index count.
- Index data is read as uploads are submitted (staging bytes copied into a buffer), or in
  place when the index buffer lives in mapped memory. Uploads small enough to be index
  buffers are kept, up to 512 MB, oldest dropped first.

Milestone 2a, meshes:

- Each mod's ini is read as EFMI's would be: components (`TextureOverride` with `hash` and
  `match_index_count`, older and `EntryPoint` formats), their LOD, and the draw list they run,
  with the toggles as they are: index buffer, `vb0`..`vb7`, `drawindexed` and
  `drawindexedinstanced`, `ps-tN` textures.
- A mod's buffers load on its first match, into one allocation in VRAM written by the CPU
  (Resizable BAR), or system memory where the GPU has no such heap. Draws that would read
  past the end of a buffer are dropped when the draw list is made, not checked per frame.
- On a matched draw: the mod's index and vertex buffers are bound, its draws issued with the
  game's instance range, the game's bindings put back, and the game's draw skipped. Before
  that, each vertex binding the game's pipeline reads must have a mod buffer of the same
  stride. When a check says no, the game's mesh is drawn and the reason logged once.
- Not yet, each logged: draws of several instances (spatial identification: 2d).
- Where a mod has no buffer for a binding the game fills with the same data as binding 0,
  its vb0 is used (older exports have no vb3; newer ones set vb3 = VB0). Where the game
  reads a shorter stride than the mod's buffer has, a copy with each element cut to it is
  made, as EFMI's own LoD 1 buffers are (UVs without the 4 bytes after them).
- A mod's component is drawn only where at least two of the mod's components are drawn at
  the same place (row 3 of the instance config), or the number the mod sets: EFMI's spatial
  identification. Small meshes several characters share stay theirs.
- Toggle keys work as in XXMI and are saved in `toggles.ini`. F9 turns the mods off and on.

Milestone 2c, textures:

- A game texture is known by 3DMigoto's DX11 hash of its upload: crc32c of mip 0, then of
  the DX11 description it would have had (default usage, shader resource, the DXGI format
  of the Vulkan format, sRGB kept). EFMI names each mod texture after the game texture it
  replaces (`... t=3156b9c5 ....dds`); `TextureOverride` sections with `this =` do the same.
- A mod's textures (DDS: BC1 to BC7, RGBA8, BGRA8, R8, RG8) load with its buffers. Their
  pixels go up at the start of one of the game's own graphics command buffers, and an event
  the GPU sets after the copies says when they can be used.
- Unity writes its descriptor sets with update templates. The layer keeps the data each set
  was last written with. For a mod's draws, a set the game has bound that holds a texture
  the mod replaces is written again into a set of the layer's own, the mod's texture in
  place of the game's, and the game's set is bound again after.
- As in EFMI on DX11 (`texture_hash = 0`, `track_texture_updates = 0`), a texture's hash is
  of the pixels it was created with; none of the mods' 223 `t=` hashes is of the description
  alone. Vulkan has no "created with" pixels, so the layer hashes mip 0 where it goes up,
  read at submit; and where it never sees them go up (memory the CPU cannot read), it reads
  mip 0 back from the GPU when a mod draw binds the texture, at the start of one of the
  game's command buffers, and hashes that. One that matches nothing is tried again later
  (its full-size mip may not have streamed in). Textures get TRANSFER_SRC for this.
- In place, as cheeky-imp (a Vulkan modding layer) does: a game texture a mod replaces by
  hash, of the same size and a copy-compatible format, gets the mod's pixels copied into it
  once, so it holds them in every pass and draw, the game's own included. Copied again when
  the game uploads into it again (streaming). The per-draw swap stays for the rest:
  textures of another size, and RabbitFX's.
- Streamed copies: Unity on Vulkan binds a smaller image of a texture while its full size
  streams in, or far away (seen: 512x512 next to the 2048x2048 one, in the same slot). Its
  mip 0 is the full texture's mip N byte for byte, so it hashes to nothing a mod names. The
  lower mips of every texture that matched are hashed too (uploads and readbacks give every
  level), and an image whose mip 0 has one of them is that texture, under its hash, in
  whichever order the two go up.
- RabbitFX (patched DX11 pixel shaders sampling a mod's diffuse, lightmap and normal map):
  the exporter names each after the game texture it stands for (t=...), which the draw
  binds in that role, so they replace by hash as the rest. (Placing them by format put them
  in global slots: a draw binds dozens of the game's textures besides its material's.)
- Not yet: `this =` overrides on the game's own draws (only mod draws swap textures), and a
  VRAM budget for mod textures.

Milestone 2b, merged skeleton:

- Endfield skins in the vertex shader. Bone matrices (3 rows of 16 bytes each) are in a big
  rolling storage buffer; where a draw's bones start is row 5 of its instance config, a
  64 KB uniform buffer. Both are in memory the CPU can read.
- A `gpu_posed` mod numbers bones in a skeleton merged from all its components; its ini
  gives each component's range and LoD remaps. EFMI builds the merged skeleton with compute
  shaders; Vulkan allows neither compute nor copies inside the render passes the draws are
  in. So the layer notes where each component's game draw reads its bones, and copies them
  into the mod's merged skeleton when the command buffer is submitted.
- Each mod draw gets a block of a 16 MB ring of the layer's own, filled at submit with its
  bones (this frame's and last frame's) and a copy of the game's config pointing at them.
  The draw's sets are bound with the bone buffer and config pointing at the ring.
- The game's pipelines read 8-bit bone indices; merged skeletons have more bones than that.
  Each bone index buffer gets a palette of at most 256 bones when the mod loads, its indices
  made 8-bit indices into it, so no pipeline of the game changes.

- Several objects drawn with the same mod (the same character twice) each get their own
  merged skeleton, told apart by the position in row 3 of their instance config. Of the
  64 KB uniform buffers a draw binds, only one whose row 5 is EFMI's (bone rows inside the
  bone buffer, then 1.0) is taken for the config: other passes bind other ones.
- `skip_skeleton_override` components give their bones to the merged skeleton and are drawn
  as their draw list says (hidden when it is empty); `draw = from_caller` draws the original.

Cross-IB bones, as many mods do with their own custom shaders (Mifu's body):

- A custom shader that reads `vs-t0` (`cs-t0 = vs-t0`) records the draw's bones under an ID,
  the `cs-t2` resource. A later draw that binds bones of its own (`vs-t0 = ...`) with an ID
  uses them: the draw's own bones when its list recorded that ID, else the bones another
  component recorded. The layer does the same without the shaders: records are kept as a
  skeleton of 256 bones per ID, filled when the recording component is drawn, and the
  draws on them go through the bone ring as merged-skeleton draws do.
- Each draw keeps the buffers bound when it was issued: lists switch to another
  component's buffers for those draws.

Later milestones: 2d spatial identification, then shape keys and `CustomShader` sections.

## Performance

What WWMI12 taught, applied from the start:

- Index data is hashed once per region, never per draw, and misses are cached too.
- A draw costs one lookup for its command buffer (each thread remembers its last) and one
  set lookup for its index count. Nothing else runs unless some mod uses that index count.
- No locks on the draw path. Uploads are read at submit, not at record time.
- The layer is the loader's own mechanism: no code patching, no injector.

## Why a layer, and when it is registered

The Vulkan loader loads every registered implicit layer into every Vulkan program, other games
with their own anti-cheats included. So `efmivulkan_launch.exe` registers the layer only for
one session:

1. adds `efmivulkan.json` under `HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers`
   (where Steam, OBS, Overwolf and Epic register theirs). HKLM, so it needs admin rights:
   Endfield runs as administrator, and in an elevated program the loader ignores layers
   registered for the current user only (`HKCU`).
2. opens the official Gryphline launcher (the game refuses to start any other way)
3. waits for `Endfield.exe` to start and to exit
4. removes the entry again, also on Ctrl+C or when its window is closed

The layer itself acts only in `Endfield.exe` and passes everything through elsewhere. Setting
`EFMIVULKAN_DISABLE=1` keeps the loader from loading it at all.

Endfield ships AntiCheatExpert. A Vulkan layer is how overlays load, but that is no promise
the anti-cheat treats this one the same.

## Keys

| Key | Action |
|---|---|
| F10 | Capture one frame, with a beep |
| F9 | Mods off or on, with a beep |
| The mods' own keys | Their toggles, while the character is on screen |

## Build

Needs Visual Studio 2022 Build Tools, CMake and git. Configuring fetches the official Vulkan
headers (KhronosGroup/Vulkan-Headers v1.3.283).

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

```bash
cmake --build build --config Release
```

## Run

1. Close the game and XXMI Launcher.
2. Run `build\Release\efmivulkan_launch.exe` and accept the admin prompt. About a minute
   after the game starts, its window says whether the layer is running in the game.
3. Start the game in the Gryphline launcher, with **Launch with DirectX 11** unticked.
4. Get modded characters on screen and press **F10** once.
5. Quit the game. The launch window unregisters the layer and closes.
6. Logs are in `%USERPROFILE%\efmivulkan`: `launch.log` (the launch tool's), `efmivulkan.log`,
   `frame.log`, `crossref.log`.

What to look for:

- `efmivulkan.log`: `ib MATCH <hash> ... <mod> [<section>]` for each component found while
  playing, and a `stats:` line every 15 s.
- `crossref.log`, `== EFMI COMPONENTS ==`: per mod, each component matched, drawn with
  different bytes (with the hashes seen), or not drawn.
- `frame.log`: every indexed draw of the captured frame with its `ibregion=`.

`efmivulkan_launch --register` and `--unregister` turn the layer on and off by hand.

## Settings

| Environment variable | Default |
|---|---|
| `EFMIVULKAN_LOG_DIR` | `%USERPROFILE%\efmivulkan` |
| `EFMIVULKAN_MODS` | `...\XXMI Launcher\EFMI\Mods` |
| `EFMIVULKAN_DISABLE` | unset. `1`: the loader skips the layer |
| `EFMIVULKAN_ANY_PROCESS` | unset. `1`: act in any process (the smoke test) |

## Layout

| Path | What |
|---|---|
| `src/layer.cpp` | The Vulkan layer: dispatch, buffers, memory, uploads, draws |
| `src/efmi.*` | EFMI matching, F10 capture, logs |
| `src/efmimod.*` | Reads an EFMI mod: components, LODs, draw lists |
| `src/efmidraw.*` | Draws mods: GPU buffers, checks, the replaced draw, toggles |
| `src/ini.*` | The 3DMigoto ini language: expressions, if blocks, [Present], keys |
| `src/ibregion.*` | Kept uploads and index region hashes |
| `src/modindex.*` | Hashes and index counts from the mods' inis |
| `src/skeleton.*`, `src/shapekeys.*` | WWMI12's skeleton and shape key code, kept for later milestones |
| `tools/launch.cpp` | Registers the layer for one session |
| `tests/` | Unit tests, and a smoke test that draws through the layer on your GPU |

## Behaviour: as DX11 EFMI

The layer follows EFMI's Core (`Core/EFMI/*.ini` and its compute shaders):

- **Spatial identification** (SpatialIdentification.ini), for mods that run
  `SpatialIdentity_IdentifyComponentInstances`: a slot per object by position (3 cells of
  0.125), expiring after 1 unseen frame; skipped on its first frame, the game's draw until
  `identification_min_components` (default 4) components were seen, then the mod's for good;
  "unknown" (the game's) after 255 frames. Mods without it draw every match.
- **Merged skeleton** (MergedSkeleton.ini): one region per identification slot; bones imported
  only at the mod's draws, once a frame per component and slot (again for a better LoD),
  scaled by `custom_mesh_scale`; row 5 offsets redirected only past the static rows; bones
  never imported are zero. Imports and fills run in recorded order, as EFMI's dispatches do.
- **Textures**: swapped per draw by hash (`CheckTextureOverride` on ps-t0..22); nothing is
  written into the game's textures, and an unidentified texture stays the game's.

Different because Vulkan has to do it differently (same result):

- Texture hashes are computed at upload, or read back from the GPU, with streamed lower-mip
  copies matched to the full texture.
- 16-bit merged-skeleton indices become 8-bit palettes (≤256 bones), where EFMI changes the
  input layout (`vb2->ElementFormat`).
- Bone copies happen on the CPU at submit, not in compute shaders inside the render pass.
- Mod vertex buffers are trimmed or padded (tangents computed) to the game's Vulkan strides.
- `if vs == N` draws stay out of outline passes, and duplicates they cause are drawn once:
  DX11 shader hashes do not exist here.

Not yet: instanced draws (more than one instance) are left to the game, where EFMI splits
them per instance; shape keys.
