# EFMI Vulkan

Run [EFMI](https://github.com/SpectrumQT/XXMI-Launcher) (XXMI) mods for **Arknights: Endfield** on
the game's **Vulkan** renderer, without switching the game to DirectX 11.

XXMI runs EFMI through 3DMigoto, which only works on DX11. EFMI Vulkan is a Vulkan layer that
does EFMI's work on Vulkan instead: it reads the same mod `.ini` files and follows the same
rules as EFMI's own Core (`XXMI Launcher\EFMI\Core\EFMI`), so mods work unchanged.

> Early release. Most mods tested work; see [Known gaps](#known-gaps). Please report what
> does not.

## Download and use

1. Download the latest zip from [Releases](../../releases) and unzip it anywhere.
2. Close the game. Run `EFMIVulkan.exe` and accept the admin prompt.
3. The first time, pick your EFMI Mods folder (`XXMI Launcher\EFMI\Mods`) and the GRYPHLINK
   folder (with `Launcher.exe`). They are saved in `config.ini`.
4. Start the game from the official launcher as usual, on Vulkan. Keep the EFMIVulkan window
   open while you play.

The layer is registered only while the game runs through `EFMIVulkan.exe`, and acts only in
`Endfield.exe`. Install mods with XXMI as usual; XXMI itself does not need to run.

| Key | Action |
|---|---|
| Shift+F8 | [Fix mods](#auto-fix) broken by a game update, from the characters on screen (also F in the EFMIVulkan window) |
| Shift+F9 | Mods on or off |
| Shift+F10 | [Reload mods](#reload), without restarting the game (also R in the EFMIVulkan window) |
| Mod keys | Each mod's own toggles, as in XXMI (remembered between runs) |

Logs: `%USERPROFILE%\efmivulkan`.

### Debug keys

For looking into a mod that does not draw right, create an empty file named `debugkeys` in
`%USERPROFILE%\efmivulkan` and start the game again. Then these keys are bound too:

| Key | Action |
|---|---|
| F10 | One frame's draws to `frame.log` and `crossref.log` |
| Ctrl+F10 | Auto-fix: keep a capture and preview what it would fix, without writing |
| Ctrl+Shift+F10 | Auto-fix: fix from the kept captures |
| Alt+M | Textures a mod replaces turn magenta |
| Alt+H | Hide what the mods draw |
| Alt+N | Hide one draw of each mod draw list (cycles) |
| Alt+B | Cycle how mod draws get their bones and config (8 modes, named in the log) |

## Auto-fix

Game updates change the hashes mods match on, which breaks mods on DX11 too. With some of the
broken characters on screen, press Shift+F8 in game (or F in the EFMIVulkan window). Mods go
off for a few frames, one frame of the game's own draws is captured and kept for the session,
and every capture kept so far fixes, per mod. Swap in the next broken characters and press it
again:

- **Meshes**: a component whose hash is no longer drawn gets the hash of the game's draw with
  the same index count on the same character (draws grouped by their bone config's position),
  if exactly one fits and no mod claims it. LoD 1 components prefer draws seen only in shadow
  passes. A component whose index count is gone was re-made by the update and is reported.
- **Textures**: a texture whose hash the game never showed gets the one unclaimed game texture
  of its size and format bound in its components' draws.
- **Texture slots**: DX11 registers (`ps-tN`) mean nothing on Vulkan. A `ps-tN` texture found
  in its own component's draw is a vote for where register N is on Vulkan (set, binding and
  set layout). Votes add up in `slots.ini` over captures. Once a register's place is clear,
  `ps-tN` lines put their texture there, which covers a texture moved into another
  component's draw.

Each changed ini is backed up next to itself (`<ini> <date>.BAK`, which no loader reads) and
rewritten, and the mods are reloaded. The report is `autofix.log`; the raw capture is
`autofix-capture.log`. Captures are kept for the game session, and grouped per capture: two
characters swapped in at the same place are never taken for one.

## Reload

Shift+F10 (or R in the EFMIVulkan window) reads the Mods folder again. Mods whose ini, buffers and
textures are unchanged stay as they are. New and edited ones load, and removed ones stop
drawing. Textures from unchanged files are shared, not loaded again. The old copies of edited
mods stay in memory until the game closes, because command buffers in flight may still use
them.

## Reporting a problem

Open an [issue](../../issues) with `efmivulkan.log` attached. The log names each mod component
it could not draw, and why. Please say whether the mod works on DX11 with XXMI: game updates
break some mods on DX11 too, until their authors update them.

## How it works

The same rules as EFMI on DX11:

- **Meshes**: components matched by index buffer region hash and `match_index_count`; each
  mod's draw list run with its toggles (`ib`, `vb0`..`vb7`, `drawindexed`, `draw = from_caller`).
- **Spatial identification** (`SpatialIdentification.ini`): objects tracked by position; an
  object's first frame is skipped, then it is the game's until enough of the mod's components
  are seen there, then the mod's.
- **Merged skeleton** (`MergedSkeleton.ini`): bones imported per object at the mod's draws, once
  a frame per component, LoD remaps and mesh scale applied.
- **Textures**: swapped per draw by 3DMigoto's texture hash (`texture_hash = 0`).

What Vulkan makes different, for the same result:

- Texture hashes are computed when the game uploads a texture, or read back from the GPU where
  the CPU never sees the pixels. Unity's smaller streamed copies are matched to the full texture.
- 16-bit merged-skeleton bone indices become 8-bit palettes of up to 256 bones, as the game's
  Vulkan pipelines read 8-bit indices.
- Bones are copied on the CPU instead of by compute shaders inside the render pass, into one
  version of the skeleton per frame: a mod draw gets the bones of the frame it was recorded in.
- Where a draw reads its bones and instance config comes from its vertex shader's SPIR-V (the
  storage and uniform buffers it declares), not from a guess at what its sets hold.
- A skinned draw that does not keep its bones the usual way (effect passes; EFMI's
  constant-buffer path) is left to the game, as EFMI leaves a mesh it cannot pose.
- A character that moves at once (a party swap, a dodge) or is unseen for a moment stays the
  mod's, with its bones, instead of being skipped as new and posed with zeros.
- Mod vertex buffers in DX11's layout are padded, trimmed or repacked to the strides and
  offsets the Vulkan pipelines read (with tangents computed where the mod has none).
- Draws under `if vs == N` are kept out of outline passes: DX11 shader hashes do not exist here.

## Known gaps

- Shape keys.
- Draws of several instances at once are left to the game (EFMI splits them per instance).
- Effect passes that keep bones in a constant buffer show the game's mesh (EFMI poses the mod there).
- CustomShader sections other than the bone recording cross-IB mods use.

## Build

Needs Visual Studio 2022 (C++), CMake and git.

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

```bash
cmake --build build --config Release
```

```bash
powershell -ExecutionPolicy Bypass -File packaging\package.ps1 -Version 0.1.0
```

The last one makes the release package in `dist\`. Tests, without the game:
`build\Release\smoketest.exe build\Release %TEMP%\efmismoke`, `build\Release\test_hash.exe`,
`build\Release\test_spirv.exe` and `build\Release\test_autofix.exe`.
`build\Release\test_parse.exe <Mods folder>` prints what each mod's ini parses to.

| Folder | Contents |
|---|---|
| `src/layer.cpp` | The Vulkan layer: hooks, descriptor and buffer tracking |
| `src/efmimod.cpp`, `src/ini.cpp` | EFMI mod ini parsing and evaluation |
| `src/efmidraw.cpp` | Matching draws, spatial identification, drawing the mod |
| `src/efmibones.cpp` | Merged skeleton |
| `src/efmitex.cpp`, `src/textures.cpp`, `src/texhash.cpp` | Textures: hashing, loading, swapping |
| `src/autofix_core.cpp`, `src/autofix.cpp` | Auto-fix: the analysis and ini rewriting; the capture |
| `tools/launch.cpp` | `EFMIVulkan.exe` |

## License

[MIT](LICENSE). Not affiliated with XXMI, EFMI or Gryphline.
