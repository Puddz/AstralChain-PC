# Astral Chain PC


> [!IMPORTANT]
> **This project was built entirely with AI.** All of the code, tooling, reverse engineering and
> documentation in this repository was written by an AI model (Anthropic's Claude, through Claude Code, and Codex), directed,
> tested and reviewed by the project's owner. It is shared as is, as a fan project. If you prefer not to use
> AI-written code, you are free to skip it. As with any code from the internet, you build and run it at your own
> discretion.

An unofficial, experimental native Windows port of *Astral Chain* (Nintendo Switch, PlatinumGames, 2019), made by
**static recompilation**. The game's ARM64 code is translated to C and compiled into an ordinary Windows x64
program, and its NVN graphics calls run on Vulkan. There is no emulator or JIT.

You build it yourself from **your own** legally dumped game and keys. This repository contains no game code, assets,
shaders, keys or firmware, and never will. Everything the setup produces comes from your dump: do not share the
`build/`, `game/` or `userdata/` folders.

> **AI disclosure.** Most of the code, tooling, reverse engineering and documentation here was written by Claude
> (Anthropic) through Claude Code, directed and tested by the repository owner. It is shared as-is as a fan project.

## Status

| | |
| --- | --- |
| Tested | Title, menus, opening cutscene, character creation, the highway chase and the first on-foot section of File 01 |
| Frame rate | 60 fps target, cutscenes included (Dynamic-FPS patch, game speed correct at any frame rate). RTX 4090 / Ryzen 7 7800X3D: 57-60 fps. `AC_CUTSCENE_FPS=30` plays cutscenes exactly as the original does |
| Stability | Driver watchdog plus a GPU hang stop. No driver resets since 2026-10-19 across dozens of test runs |
| Saving | Works (tested in File 01) |
| Not yet tested | Everything after the first on-foot section of File 01 |

Known issues are listed under [Known issues](#known-issues).

## Requirements

- Windows 10 or 11, 64-bit.
- A GPU with Vulkan 1.3. Developed on an NVIDIA RTX 4090; other GPUs are untested.
- [Python](https://www.python.org/downloads/windows/) 3.12 or newer, with the `py` launcher (the installer default).
- About **35 GB** free during setup. About 11 GB stays afterwards.
- An internet connection for the one-time setup. It downloads Zig, glslang and NSTool at pinned versions,
  checked by SHA-256.
- Your own dump:
  - the **base game** (title ID `01007300020FA000`), as an XCI or an NSP;
  - the **v1.0.1 update** NSP. Exactly v1.0.1 is required;
  - your console's **`prod.keys`**.

How to dump games and keys from your Switch is not covered here.

## Setup

1. Download this repository: **Code → Download ZIP**, or `git clone`. Put it in a folder you own, for example
   `C:\Games\AstralChain-PC`. The path should be short, and not on a network drive.
2. Double-click **`setup.cmd`** and pick, when asked:
   1. your `prod.keys`;
   2. the base game (XCI or NSP);
   3. the v1.0.1 update NSP.
3. Wait. Setup extracts the game data, packs it into `game\romfs.bin`, then translates and compiles the game. The
   first time takes roughly **15-40 minutes**, depending on your CPU and disk. Temporary files are removed at the
   end.

If a step fails, fix the cause and run `setup.cmd` again: finished steps are skipped. You can also pass the files
directly: `setup.cmd --keys X:\prod.keys --base X:\AstralChain.xci --update X:\AstralChain-v1.0.1.nsp`.

## Playing

Double-click **`Play Astral Chain.cmd`**.

| Switch | Keyboard |
| --- | --- |
| A / B / X / Y | J / K / I / U |
| L / R | Q / E |
| ZL / ZR | Z / C |
| + / - | Enter / Backspace |
| D-pad | Arrow keys |
| Left stick | W A S D |
| Right stick | T F G H |

XInput controllers (Xbox pads and compatibles) work too. The game pauses while its window is not focused, as on
the Switch's home menu.

- **Saves** are in `userdata\saves\`. Back that folder up from time to time.
- **Logs** of the last session are in `build\game\round-00\` (`trace.log`, `stderr.log`).
- **New shaders.** The first time a scene is seen, its shaders are compiled, and the game may pause briefly, up
  to a few seconds on a loading screen. Later sessions reuse the cache in `build\game\cache\`.

### Safety

The game runs under `astral_chain\run_guarded.ps1`. It closes the game on the first GPU driver error that Windows
logs. The game also stops itself if the GPU stays busy for more than 1.2 s, before Windows would reset the driver.

### Updating

Download the new version over the old one, or `git pull`. The next launch of `Play Astral Chain.cmd` rebuilds the
game once, in about a minute. Your saves, game data and shader cache are kept.

## Known issues

- Defeated enemies may leave dark remnants in the highway chase. A likely cause is fixed, but the fix is
  unconfirmed.
- Cutscene audio was reported out of sync (the cutscene just before character creation, which started with a loading
  pause). Since then textures and shaders are prepared ahead of time (no pause at its start in tests), and the 60 fps
  cutscene keeps the same pace as the unmodified game (measured, within 0.1 s over 48 s). Not yet confirmed to fix it.
- Save-slot thumbnails are not captured (`nvnTextureReadTexels` is not implemented).

Reports are welcome. Include `build\game\round-00\stderr.log` and your GPU and driver version, but no game files.

## Folder layout

| Path | What |
| --- | --- |
| `setup.cmd`, `Play Astral Chain.cmd` | Entry points |
| `astral_chain/` | Translator driver, build system and Windows runtime (C) |
| `astral_chain/patches/` | Code patches of this project (pchtxt) |
| `third_party/` | Vendored sources: LASRecomp, Vulkan-Headers, VulkanMemoryAllocator, Dynamic-FPS patch |
| `docs/` | How it works, troubleshooting, development notes |
| `tools/` | *(created by setup)* Zig, glslang, NSTool |
| `game/` | *(created by setup)* your extracted update code, metadata and `romfs.bin` |
| `build/` | *(created by setup)* the compiled game and its shader cache |
| `userdata/` | *(created by setup)* saves, account profile, setup choices |

## Documentation

- [How it works](docs/how-it-works.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Development notes](docs/development.md)

## Legal

This is an unofficial fan project, not affiliated with or endorsed by Nintendo or PlatinumGames. *Astral Chain* is
a trademark of Nintendo. No game code, assets, shaders, keys or firmware are distributed. Requests for them will be
closed.

The original code in this repository is under the MIT License (see `LICENSE`). Vendored third-party components keep
their own licenses, listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Credits

- [LASRecomp](https://github.com/peiikai/LASRecomp) (MIT): the ARM64 lifter, the NVN-on-Vulkan layer and the
  Maxwell shader translator this port builds on.
- [AstralChain-Dynamic-FPS](https://github.com/yogesh1239/AstralChain-Dynamic-FPS) by yogesh1239 (MIT): the
  60 fps patch.
- [astral-extractor](https://github.com/cabalex/astral-extractor) by cabalex: format research.
- [AC-Recomp](https://github.com/swaymun/AC-Recomp): the macOS recompilation, whose setup flow inspired this one.
- [NSTool](https://github.com/jakcron/nstool), [Zig](https://ziglang.org/),
  [glslang](https://github.com/KhronosGroup/glslang), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers),
  [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
  [Capstone](https://www.capstone-engine.org/), [lz4](https://github.com/python-lz4/python-lz4).
