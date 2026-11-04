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
| F9 | Mods on or off |
| Mod keys | Each mod's own toggles, as in XXMI (remembered between runs) |
| Alt+M | Debug: textures a mod replaces turn magenta |
| Alt+H | Debug: hide what the mods draw |

Logs: `%USERPROFILE%\efmivulkan`.

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
- Bones are copied on the CPU instead of by compute shaders inside the render pass.
- Mod vertex buffers in DX11's layout are padded, trimmed or repacked to the strides and
  offsets the Vulkan pipelines read (with tangents computed where the mod has none).
- Draws under `if vs == N` are kept out of outline passes: DX11 shader hashes do not exist here.

## Known gaps

- Shape keys.
- Draws of several instances at once are left to the game (EFMI splits them per instance).
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
`build\Release\smoketest.exe build\Release %TEMP%\efmismoke` and `build\Release\test_hash.exe`.
`build\Release\test_parse.exe <Mods folder>` prints what each mod's ini parses to.

| Folder | Contents |
|---|---|
| `src/layer.cpp` | The Vulkan layer: hooks, descriptor and buffer tracking |
| `src/efmimod.cpp`, `src/ini.cpp` | EFMI mod ini parsing and evaluation |
| `src/efmidraw.cpp` | Matching draws, spatial identification, drawing the mod |
| `src/efmibones.cpp` | Merged skeleton |
| `src/efmitex.cpp`, `src/textures.cpp`, `src/texhash.cpp` | Textures: hashing, loading, swapping |
| `tools/launch.cpp` | `EFMIVulkan.exe` |

## License

[MIT](LICENSE). Not affiliated with XXMI, EFMI or Gryphline.
