# Development notes

## Building by hand

`setup_game.py` runs this command, with paths relative to the repository:

```powershell
.venv\Scripts\python -m astral_chain.native_trace game\update-exefs --reference third_party\LASRecomp `
  --compiler tools\zig\zig.exe --output build\<new folder> --instructions 4000000 --sample-steps 0 `
  --run-timeout 86400 --unbounded --chain --follow 0 --entry rtld:start --seed-all-text --build cached `
  --control game\control.nacp --romfs game\romfs.bin --nvn-hle --nvn-backend vulkan `
  --optimize --lean --direct-calls --jumps `
  --patch third_party\AstralChain-Dynamic-FPS\1.0.1.pchtxt --patch astral_chain\patches\cutscene-catchup-1.0.1.pchtxt `
  --build-only
powershell -File astral_chain\run_guarded.ps1 -Run build\<new folder> -Seconds 300 -Env "AC_PARALLEL=1"
```

- `native_trace` refuses an existing output folder.
- A full translation and compile takes about 5 minutes. With unchanged code units, a runtime-only change takes
  about 1 minute.
- Each build writes `run-env.txt`, holding addresses that move with `main`'s size, and `run_guarded.ps1` reads
  it.
- Always run through `run_guarded.ps1`.

## Unattended test runs

- **Scripted input.** `AC_INPUT_SCRIPT` takes `t0-t1:mask[@period/duty];...`, in seconds after the first input
  sample. `python -m astral_chain.input_scripts highway|explore <s>|wander <s>` prints ready-made scripts.
  - Mask bits: A `0x1`, B `0x2`, ZL `0x100`, ZR `0x200`, + `0x400`, D-pad left/up/right/down `0x1000-0x8000`.
  - Left stick bits: left/up/right/down `0x10000-0x80000`. The right stick uses `<<4` of those.
- **Separate data.** `AC_DATA_DIR=<folder>` replaces the build's code images (`images\`), `romfs.bin` and save
  root (`save\`). Use it to test without touching your saves. Give the real path: the save store rejects paths
  through junctions or `subst` drives.
- **Screenshots.**
  - `LARECOMP_SCREENSHOT_EVERY=<frames>` saves `frame_N.png` in the run folder.
  - Add `LARECOMP_SCREENSHOT_FROM=<frame>` to save into `frames\` instead.
  - `LARECOMP_SHOT_AT=<frame>,...` captures specific frames.
- **Pass dumps.** `LARECOMP_DEBUG_FRAME=<frame>` writes every render pass of that frame to `build\pass_NNN.png`
  and logs each draw.

## Diagnostics

| Variable | Effect |
| --- | --- |
| `LARECOMP_VK_PROF=1` | Per-2-s CPU/GPU breakdown, texture upload and tracking counters, vertex cache stats |
| `LARECOMP_GPUPROF_AT=<frame>` / `LARECOMP_GPUPROF_DRAWS=1` | GPU time per pass, or per draw, of a frame |
| `LARECOMP_GPUPROF_AUTO=1` | Also profile the frame after each frame slower than 250 ms (up to 12; profiling makes a frame slow) |
| `AC_CUTSCENE_FPS=30` | Cutscenes as in the unmodified game: its fixed-step timer path at 30 FPS (default: 60 FPS, measured step) |
| `AC_PROFILE=1` (+ `AC_PROFILE_THREAD=N`) | CPU sampling profiler. Report: `python -m astral_chain.profile_report <run>` |
| `AC_PARALLEL_STATS=1` | Guest thread scheduling stats |
| `AC_AUDIO_DUMP=<file>` | Raw PCM of everything played. Report: `python -m astral_chain.audio_report <run>` (needs `requirements-dev.txt`) |
| `LARECOMP_SAVE_MSL=1` | Translated GLSL and translator inputs in `build\glsl`. Replay: `python -m astral_chain.shader_replay` |
| `AC_TRACE_CALLS=1` | Per-call SVC/IPC/audio/HID log lines (off in play builds) |
| `LARECOMP_SKIP_UNBUILT=1` | Skip draws whose pipeline is still compiling, instead of waiting for it |
| `LARECOMP_TEX_FULLHASH=1` | Hash every texture fully at each check (slow; rules change detection in or out) |
| `AC_GPU_HANG_MS=<ms>` | GPU hang stop threshold (default 1200) |
| `LARECOMP_DUMP_TEX=<min width>` | Level-0 data of up to 24 textures (as uploaded, unswizzled) to `tex_*.bin` in the run folder |
| `LARECOMP_DUMP_ASTC=<min width>` | Blocks and CPU decode of up to 16 ASTC textures (GPUs without ASTC only) to `astc_*.bin` |
| `LARECOMP_LOG_PACKAGED=1` | Log each texture's packaged layout (block height) |
| `AC_CUTSCENE_STALL=<n>:<s>:<ms>[:<s>:<ms>...]` | Stall the game thread `<ms>` ms, `<s>` s after the n-th cutscene starts (A/V sync tests) |
| `LARECOMP_STALLS=<frame>:<ms>,...` | Stall the render thread at those frames; `LARECOMP_SLOW_FRAME_MS` / `_FROM` / `_UNTIL` / `_EVERY` for ranges |
| `AC_CUTSCENE_STALL_LOCKED=1` | Test stalls hold the kernel lock: every guest thread waits, the sound thread too |
| `AC_CUT_CARRY=0` | Cutscene cuts end as in the original code (each cut up to a frame early at 60 fps; svc #0xB2) |
| `AC_AUDIO_RESYNC=0` | Do not skip sound after an underrun (by default the gap, up to 0.3 s, is skipped to stay in sync) |
| `AC_SAVE_SNAPSHOTS=0` | Do not copy each game save to `<save folder>-snapshots\<date-time>\` (on by default, newest 30 kept) |

With `LARECOMP_VK_PROF=1`, frames over 50 ms are logged as `[hitch]` lines with their time split (texture uploads,
waits for shader programs). `trace.log` always gets `CUTSCENE` lines (cutscene start and end, frames over 200 ms in a
cutscene) and `AUDIO underrun` lines when the sound queue ran dry.

### Testing from a checkpoint

Game saves work as checkpoints: every save the game writes is also copied to `<save folder>-snapshots\<date-time>\`.
To replay a moment, copy a snapshot's `data` folder over a test save folder (`AC_DATA_DIR=<folder>`, see above) and
load it. The game's own autosaves usually come right before story cutscenes.

## Code map

| File | Role |
| --- | --- |
| `setup_game.py` | Setup and rebuild-on-update |
| `native_trace.py` | Loads the NSOs, applies patches, translates, generates C, builds, writes `run-env.txt` |
| `native_build.py`, `nvn_build.py`, `shader_build.py` | Cached compilation of code units, the NVN layer and the patched shader translator |
| `shader_cache.py`, `runtime/shader_precompile.c` | Shader precompilation: extracts the shaders from `root.pkz` to `game\shaders`, compiles every one of them (generic draw-time keys) plus the variants of `data/shader_variants.bin` and `cache/programs.bin` into `cache/spirv` |
| `data/shader_variants.bin` | Known shader variants (code hashes and draw-time keys, `programs.bin` format; no game data). Grows from test runs |
| `runtime/host_runtime.c.in` | Kernel and services runtime |
| `runtime/guest_threads.h` | Guest threads and the parallel scheduler |
| `runtime/nvn_hle.h` | NVN entry points |
| `runtime/las/nvn/vulkan/` | Renderer: `exec.c` commands and draws, `textures.c` uploads, `shaders.c` SPIR-V cache, `device.c` swapchain |
| `runtime/las/maxwell_ext.h` | Maxwell instructions added to the translator (LDS/STS, atomics, VOTE, SUST, TLD...) |
