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
  --patch astral_chain\patches\resolution-1.0.1.pchtxt `
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
| `LARECOMP_GPUPROF_EVERY=<frames>` | Also profile one frame every that many frames (`[gpuprof] frame N` header; each profiled frame stalls briefly): for players' reports |
| `LARECOMP_GPUPROF_AUTO=1` | Also profile the frame after each frame slower than 250 ms (up to 12; profiling makes a frame slow) |
| `AC_RESOLUTION=<w>x<h>` | (launcher: *Render resolution*) docked render size, up to 8.6 million pixels (3840x2160), no dynamic resolution; wider than 16:9 (e.g. 2560x1080, 3840x1080) widens the 3D cameras' aspect and the window; the render target pool grows by 160 bytes per extra pixel (`resolution-1.0.1.pchtxt`, svc #0xB3 in host_runtime.c.in) |
| `AC_UPSCALER=fsr1` | (launcher: *Upscaling*) AMD FSR 1 (EASU upscale, RCAS sharpen; exec.c `post_process`) when the picture is shown larger than it is rendered; `AC_FSR_SHARPNESS=<stops>` (0 strongest - 2, default 0.2) |
| `AC_FULLSCREEN=1` | (launcher: *Start in fullscreen*) Borderless fullscreen on the window's monitor from the start (`app_window.h` `app_set_fullscreen`, also F11 / Alt+Enter) |
| `AC_HANDHELD=1` | (launcher: *Handheld mode*) The game is told it runs handheld (operation mode 0, normal performance mode; `app_window.h`): it renders 1280x720 into its 1600x900 window textures and crops to it (`nvnWindowSetCrop`, honoured by the present in `device.c`) |
| `AC_CUTSCENE_FPS=30` | Cutscenes as in the unmodified game: its fixed-step timer path at 30 FPS (default: 60 FPS, measured step) |
| `AC_PROFILE=1` (+ `AC_PROFILE_THREAD=N`) | CPU sampling profiler. Report: `python -m astral_chain.profile_report <run>` |
| `AC_PARALLEL_STATS=1` | Guest thread scheduling stats |
| `AC_AUDIO_DUMP=<file>` | Raw PCM of everything played. Report: `python -m astral_chain.audio_report <run>` (needs `requirements-dev.txt`) |
| `LARECOMP_SAVE_MSL=1` | Translated GLSL and translator inputs in `build\glsl`. Replay: `python -m astral_chain.shader_replay` |
| `AC_TRACE_CALLS=1` | Per-call SVC/IPC/audio/HID log lines (off in play builds) |
| `LARECOMP_SKIP_UNBUILT=1` | Skip draws whose pipeline is still compiling, instead of waiting for it |
| `LARECOMP_TEX_FULLHASH=1` | Hash every texture fully at each check (slow; rules change detection in or out) |
| `AC_ASYNC_SUBMIT=1` | (launcher: *Prepare graphics on a separate thread*) Queue submissions, fences and presents to a worker thread that runs the Vulkan backend (`objects.c`); other NVN calls wait for the queue unless `nvn_async_safe`. The profile line counts the waits (`async drains`, `fence waits`) |
| `LARECOMP_VK_WW_BATCH=0` | One write tracking query per texture check, as before the batched checks (`exec.c` `ww_written_pages`); the profile line's `write watch` part counts the queries by caller |
| `AC_GPU_HANG_MS=<ms>` | GPU hang stop threshold (default 1200) |
| `LARECOMP_DUMP_TEX=<min width>` | Level-0 data of up to 24 textures (as uploaded, unswizzled) to `tex_*.bin` in the run folder |
| `LARECOMP_DUMP_ASTC=<min width>` | Blocks and CPU decode of up to 16 ASTC textures (with `LARECOMP_GPU_ASTC=0`) to `astc_*.bin` |
| `LARECOMP_GPU_ASTC=0` | Decode ASTC on the CPU, as before the GPU decoder |
| `LARECOMP_BC7=1` | (launcher: *Texture compression*) Store ASTC textures as BC7 (`bc7_gpu.c`): precomputed ones from `cache\bc7` if there, the others recompressed on the GPU. Logs `[vk] BC7: ...`; the 2-s profile line shows `video memory` and the precomputed hits / misses |
| `AC_BC7_PRECOMPUTE=1` | (setup; launcher: *prepared at setup*) `setup_game` encodes every ASTC texture to BC7 into `build\game\cache\bc7` (`bc7_cache.py`, once) |
| `LARECOMP_BC7_CACHE=0` | Ignore the precomputed BC7 textures (GPU conversion only) |
| `LARECOMP_BC7_LOG=1` | One `[bc7key]` line per ASTC texture upload: hash, layout, precomputed hit or miss |
| `AC_KEYS=A:74,B:75,...` | (launcher **Controls**) Keyboard keys as Windows virtual-key codes: `A B X Y LSTICK RSTICK L R ZL ZR PLUS MINUS DLEFT DUP DRIGHT DDOWN LLEFT LUP LRIGHT LDOWN RLEFT RUP RRIGHT RDOWN` (`hid_service.h`); unlisted ones keep their default, 0 for none |
| `LARECOMP_ASTC_VERIFY=<n>` | Read back the first n texture levels the GPU decodes and compare them with the CPU decoder (`ASTC verify` lines; slow) |
| `LARECOMP_NO_PIPE_WARM=1` | No pipeline warm-up (pipelines are built at their first draw only) |
| `LARECOMP_GPL=0` | No shader libraries (`VK_EXT_graphics_pipeline_library`): pipelines are compiled whole at their first draw |
| `LARECOMP_NO_GPL_WARM=1` | Shader libraries only when a program is built, not when the game loads a shader |
| `AC_GPU_WARM=0` | (launcher) Skip the GPU warm-up before the game (`shader_cache.warm_gpu`, exec.c `vk_gpu_warm`) |
| `__GL_SHADER_DISK_CACHE_PATH=<empty folder>` | (NVIDIA) Start the driver's shader cache empty, for this process only: first-play measurements without touching the shared cache |
| `LARECOMP_DEBUG_IF_DARK=<x0>,<y0>,<x1>,<y1>,<level>[,<frame>]` | With screenshots: debug-dump the next frame once a screenshot is darker than level in that box (fractions of the window) |
| `LARECOMP_LOG_PACKAGED=1` | Log each texture's packaged layout (block height) |
| `AC_CUTSCENE_STALL=<n>:<s>:<ms>[:<s>:<ms>...]` | Stall the game thread `<ms>` ms, `<s>` s after the n-th cutscene starts (A/V sync tests) |
| `LARECOMP_STALLS=<frame>:<ms>,...` | Stall the render thread at those frames; `LARECOMP_SLOW_FRAME_MS` / `_FROM` / `_UNTIL` / `_EVERY` for ranges |
| `AC_CUTSCENE_STALL_LOCKED=1` | Test stalls hold the kernel lock: every guest thread waits, the sound thread too |
| `AC_CUT_CARRY=0` | Cutscene cuts end as in the original code (each cut up to a frame early at 60 fps; svc #0xB2) |
| `AC_AUDIO_RESYNC=0` | Do not skip sound after an underrun (by default the gap, up to 0.3 s, is skipped to stay in sync) |
| `AC_SAVE_SNAPSHOTS=0` | Do not copy each game save to `<save folder>-snapshots\<date-time>\` (on by default, newest 30 kept) |
| `AC_CRASH_DUMP=1` | On a host fault, also write `crash.dmp` (a minidump: thread stacks and registers) to the run folder. Read it with `round-00\trace.pdb`. The launcher's diagnostics mode sets it |

With `LARECOMP_VK_PROF=1`, frames over 50 ms are logged as `[hitch]` lines with their time split (texture uploads,
waits for shader programs). The 2-second lines count pipeline cache hits and misses. Frames with over 40 ms of
texture work get a `texture time` line (hashing, uploads, ASTC decoded in the frame, in the background or on the
GPU), and any single texture upload over 20 ms gets a `slow texture upload` line, profiling or not. The pipeline
warm-up reports `pipeline warm-up queued / ready / late / rejected` (ready: built before its first draw), the shader
libraries `shader libraries built / reused / linked / optimized`.

`python -m astral_chain.astc_gpu_test [astc_*.bin...]` checks the GPU ASTC decoder against the CPU one, byte for
byte: random blocks of every footprint, sRGB and not, plus any texture dumps given. `python -m
astral_chain.bc7_gpu_test [astc_*.bin...]` measures the BC7 encoder (PSNR on test images and dumps; every block must
be mode 6). `trace.log` always gets `CUTSCENE` lines (cutscene start and end, frames over 200 ms in a
cutscene) and `AUDIO underrun` lines when the sound queue ran dry.

### Testing from a checkpoint

Game saves work as checkpoints: every save the game writes is also copied to `<save folder>-snapshots\<date-time>\`.
To replay a moment, copy a snapshot's `data` folder over a test save folder (`AC_DATA_DIR=<folder>`, see above) and
load it. The game's own autosaves usually come right before story cutscenes.

## BC7 textures

`LARECOMP_BC7=1` stores the game's ASTC textures as BC7 images, a quarter of RGBA8's video memory. A texture gets
its BC7 blocks one of two ways:

- **Precomputed** (`AC_BC7_PRECOMPUTE=1` at setup, `bc7_cache.py` and `runtime/bc7_convert.c`): every texture of
  the game's packs (XT1 headers in `.wta`, data in `.wtp`, inside `.dat`/`.dtt`/`.evn`/`.eff` packs or loose in the
  `.pkz` archives; WTB files hold both) is unswizzled with the runtime's own layout code, decoded, and encoded with
  bc7enc (modes 1/5/6/7, quality 1) on all CPU threads. `cache/bc7/index.bin` lists them by the same hash
  `upload_texture` computes over the texture's guest bytes (`bc7_cache.h`); `data.bin` holds the blocks. Some
  textures are created with their mipmaps only down to one ASTC block: they get a second index entry for the same
  data. 13,387 textures, 16.2 GB, 12.4 min on a Ryzen 7 7800X3D; every texture of the highway route found.
- **On the GPU** (`bc7_gpu.c`, anything not precomputed): the ASTC decoder writes the level into a scratch buffer, a
  compute shader encodes BC7 mode 6 blocks (principal axis endpoints, least squares refinement), and they are copied
  into the image. Levels over 4096x4096 stay RGBA8.

Comparison (2026-10-21; fresh install, first-play state for each run, highway chase, 3 runs each, interleaved; RTX
4090; pauses counted between frames 1000 and 19950):

| | Off | GPU | Precomputed |
| --- | --- | --- | --- |
| Video memory, average / peak | 7.2 / 8.7 GB | 4.7 / 5.2 GB | 4.7 / 5.1 GB |
| Pauses per run (total) | 8, 6, 7 (~460 ms) | 12, 12, 11 (~980 ms) | 7, 6, 6 (~500 ms) |
| Texture upload time per run | 9.4 s | 9.2 s | 10.4-11.3 s (reads; 4 uploads of ~26 ms, one 101 ms pause) |
| PSNR on 300 game textures, mean / worst | | 46.2 / 27.2 dB | 51.0 / 34.2 dB |

The GPU version's extra pauses (60-100 ms, mostly on the loading screen as an area's textures arrive) are counted as
"other" time, not texture uploads or GPU waits: not explained yet. Its quality gap is in non-colour textures
(normal maps, masks: 43.4 vs 49.6 dB; colour 50.5 vs 53.2 dB): mode 6 alone has one line through RGBA per block.
The precomputed runs had most of the 16 GB file in the Windows file cache (31 GB RAM); from a cold disk its reads
would take longer. `python -m astral_chain.bc7_cache --keys <file>` lists the precomputed keys without encoding.

## Code map

| File | Role |
| --- | --- |
| `setup_game.py` | Setup and rebuild-on-update; `--status` (JSON) and `--force-shaders` for the launcher |
| `native_trace.py` | Loads the NSOs, applies patches, translates, generates C, builds, writes `run-env.txt` |
| `native_build.py`, `nvn_build.py`, `shader_build.py` | Cached compilation of code units, the NVN layer and the patched shader translator |
| `bc7_cache.py`, `runtime/bc7_convert.c` | The optional BC7 preparation (`AC_BC7_PRECOMPUTE=1`): game textures to `build\game\cache\bc7` |
| `shader_cache.py`, `runtime/shader_precompile.c` | Shader precompilation: extracts the shaders from `root.pkz` to `game\shaders`, compiles every one of them (generic draw-time keys) plus the variants of `data/shader_variants.bin` and `cache/programs.bin` into `cache/spirv` |
| `data/shader_variants.bin` | Known shader variants (code hashes and draw-time keys, `programs.bin` format; no game data). Grows from test runs |
| `data/pipeline_variants.bin` | Known pipelines (a program's code hashes and keys, and the render state; `pipelines.bin` format; no game data). Merged into the cache at launch. Grows from test runs |
| `runtime/host_runtime.c.in` | Kernel and services runtime |
| `runtime/guest_threads.h` | Guest threads and the parallel scheduler |
| `runtime/nvn_hle.h` | NVN entry points |
| `runtime/las/nvn/vulkan/` | Renderer: `exec.c` commands, draws and the pipeline warm-up, `textures.c` uploads, `astc_gpu.c` ASTC decoding on the GPU, `bc7_gpu.c` the optional BC7 recompression, `shaders.c` SPIR-V cache, `device.c` swapchain |
| `runtime/las/maxwell_ext.h` | Maxwell instructions added to the translator (LDS/STS, atomics, VOTE, SUST, TLD...) |

## The launcher

`Astral Chain Launcher.exe` is built from `launcher/Launcher.cs` by `python launcher\build.py`, with the C# 5
compiler of the .NET Framework 4 that ships with Windows. Rebuild and commit the exe whenever the source changes.
The backgrounds in `launcher/art/` (every `.jpg`, in file name order; the first is the default) are embedded in the
exe and picked with the arrows at the top right (`background=` in `launcher.ini`); `launcher/icon.ico` is its icon; the window is
borderless and draws its own buttons (`Theme`, `NeonButton`, `GlyphButton`, `NeonProgress`).

- It runs the same steps as the .cmd files: `.venv` and `requirements.txt`, then `setup_game` (install;
  `--rebuild-if-stale` before each start; `--force-shaders` for **Compile shaders** when already compiled), then
  `run_guarded.ps1`. Everything runs without windows (`-NoConsole`: the game shares the guard script's hidden
  console); its output goes to the launcher's log and `userdata\launcher.log`.
- The button states come from `setup_game --status`: `installed` (the game data is extracted), `build`
  (`current`, `stale`, `missing`) and `shaders` (`compiled`, `needed`, `never`: `shader_cache.status` compares the
  precompile and warm-up stamps with what a launch would write; a new driver is only noticed by the warm-up).
- Settings are stored in `userdata\launcher.ini` and become the variables of the tables above. The option list is
  `Options` in `Launcher.cs`.
- **Controls** (`ControlsForm`) stores `keys=A:74,...` (virtual-key codes, as the game window receives them) and
  passes `AC_KEYS` only when a key differs from the defaults. Mapped Alt and F10 no longer open the game window's
  system menu.
- Diagnostics mode adds `LARECOMP_VK_PROF=1` and `AC_CRASH_DUMP=1`. **Create report** zips `run.txt`,
  `stderr.log`, `trace.log` (last 8 MB each), `launcher.log`, `crash.dmp` and a system summary into `reports\`,
  with the Windows user name replaced in paths.
