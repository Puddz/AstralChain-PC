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
  (create `build\` in the run folder first) and logs each draw with its shader hashes.
  - `LARECOMP_DEBUG_EACH=1` ends the pass before every draw, so each draw's result is its own image; the log says
    which image holds the state before each draw.
  - Shader names for the hashes: `AC_PRECOMPILE_NAMES=1 build\shader-precompile\<id>\shader_precompile.exe
    game\shaders 1`. `AC_PRECOMPILE_DUMP=<name part>` prints the translated GLSL instead.
  - `LARECOMP_SKIP_FSHASH=<hash>,...` skips draws by fragment shader hash. `LARECOMP_WATCH_FS=<hash>` logs that
    shader's draws in every frame (blend, alpha test, constant buffers); `LARECOMP_WATCH_DEBUG=<n>` debug-dumps the
    frame of its n-th draw.

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
| `LARECOMP_DUMP_ASTC=<min width>` | Blocks and CPU decode of up to 16 ASTC textures (with `LARECOMP_GPU_ASTC=0`) to `astc_*.bin` |
| `LARECOMP_GPU_ASTC=0` | Decode ASTC on the CPU, as before the GPU decoder |
| `LARECOMP_ASTC_VERIFY=<n>` | Read back the first n texture levels the GPU decodes and compare them with the CPU decoder (`ASTC verify` lines; slow) |
| `LARECOMP_NO_PIPE_WARM=1` | No pipeline warm-up (pipelines are built at their first draw only) |
| `LARECOMP_LOG_PACKAGED=1` | Log each texture's packaged layout (block height) |
| `AC_CUTSCENE_STALL=<n>:<s>:<ms>[:<s>:<ms>...]` | Stall the game thread `<ms>` ms, `<s>` s after the n-th cutscene starts (A/V sync tests) |
| `LARECOMP_STALLS=<frame>:<ms>,...` | Stall the render thread at those frames; `LARECOMP_SLOW_FRAME_MS` / `_FROM` / `_UNTIL` / `_EVERY` for ranges |
| `AC_CUTSCENE_STALL_LOCKED=1` | Test stalls hold the kernel lock: every guest thread waits, the sound thread too |
| `AC_CUT_CARRY=0` | Cutscene cuts end as in the original code (each cut up to a frame early at 60 fps; svc #0xB2) |
| `AC_AUDIO_RESYNC=0` | Do not skip sound after an underrun (by default the gap, up to 0.3 s, is skipped to stay in sync) |
| `AC_SAVE_SNAPSHOTS=0` | Do not copy each game save to `<save folder>-snapshots\<date-time>\` (on by default, newest 30 kept) |

With `LARECOMP_VK_PROF=1`, frames over 50 ms are logged as `[hitch]` lines with their time split (texture uploads,
waits for shader programs). The 2-second lines count pipeline cache hits and misses. Frames with over 40 ms of
texture work get a `texture time` line (hashing, uploads, ASTC decoded in the frame, in the background or on the
GPU), and any single texture upload over 20 ms gets a `slow texture upload` line, profiling or not. The pipeline
warm-up reports `pipeline warm-up queued / ready / late / rejected` (ready: built before its first draw).

`python -m astral_chain.astc_gpu_test [astc_*.bin...]` checks the GPU ASTC decoder against the CPU one, byte for
byte: random blocks of every footprint, sRGB and not, plus any texture dumps given. `trace.log` always gets `CUTSCENE` lines (cutscene start and end, frames over 200 ms in a
cutscene) and `AUDIO underrun` lines when the sound queue ran dry.

### Testing from a checkpoint

Game saves work as checkpoints: every save the game writes is also copied to `<save folder>-snapshots\<date-time>\`.
To replay a moment, copy a snapshot's `data` folder over a test save folder (`AC_DATA_DIR=<folder>`, see above) and
load it. The game's own autosaves usually come right before story cutscenes.

## Ideas for later

### Optional ASTC to BC7 conversion at setup

Most of the game's textures are ASTC, which desktop NVIDIA and AMD GPUs cannot sample. They are now decoded by a
compute shader into RGBA8 (`astc_gpu.c`). Before that, the CPU decoded them, and on the highway chase (RTX 4090,
Ryzen 7 7800X3D, 2026-10-21) that caused 4 hitches of 58-236 ms (frame 9494: 45 textures, 177 ms); with the GPU
decoder no frame of the same route spends 40 ms on textures.

What remains: the decoded textures take 4 bytes per texel in video memory, 16x ASTC 8x8 (a 4096x4096 texture: ~90
MB with its mipmaps instead of ~5.3 MB). An optional setup step could convert every ASTC texture once into BC7, which
these GPUs sample directly, at about the size of the ASTC data: a quarter of today's video memory for these
textures, which matters on GPUs with 8 GB or less:

- Find the textures in the game's archives (WTA/WTP pairs in the `.dat`/`.dtt` packs; see astral-extractor) and
  record each one's layout (format, size, levels, swizzle).
- Decode each one (the existing decoder), encode to BC7 (for example with bc7enc, MIT), and store the result in
  `build/game/cache/bc7/`, keyed by a hash of the ASTC data.
- At upload, look the texture up by the same hash. A hit uploads BC7 into a BC7 image (no decode, a quarter of
  RGBA8's memory); a miss falls back to today's path.

Expected cost: BC7 encoding of the ~10 GB of game data takes long (likely tens of minutes on 16 threads at a
reasonable quality setting) and needs a few GB of disk. So it should be offered as a choice in setup, not forced.

## Code map

| File | Role |
| --- | --- |
| `setup_game.py` | Setup and rebuild-on-update |
| `native_trace.py` | Loads the NSOs, applies patches, translates, generates C, builds, writes `run-env.txt` |
| `native_build.py`, `nvn_build.py`, `shader_build.py` | Cached compilation of code units, the NVN layer and the patched shader translator |
| `shader_cache.py`, `runtime/shader_precompile.c` | Shader precompilation: extracts the shaders from `root.pkz` to `game\shaders`, compiles every one of them (generic draw-time keys) plus the variants of `data/shader_variants.bin` and `cache/programs.bin` into `cache/spirv` |
| `data/shader_variants.bin` | Known shader variants (code hashes and draw-time keys, `programs.bin` format; no game data). Grows from test runs |
| `data/pipeline_variants.bin` | Known pipelines (a program's code hashes and keys, and the render state; `pipelines.bin` format; no game data). Merged into the cache at launch. Grows from test runs |
| `runtime/host_runtime.c.in` | Kernel and services runtime |
| `runtime/guest_threads.h` | Guest threads and the parallel scheduler |
| `runtime/nvn_hle.h` | NVN entry points |
| `runtime/las/nvn/vulkan/` | Renderer: `exec.c` commands, draws and the pipeline warm-up, `textures.c` uploads, `astc_gpu.c` ASTC decoding on the GPU, `shaders.c` SPIR-V cache, `device.c` swapchain |
| `runtime/las/maxwell_ext.h` | Maxwell instructions added to the translator (LDS/STS, atomics, VOTE, SUST, TLD...) |
