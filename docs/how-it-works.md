# How it works

## From your dump to `trace.exe`

1. **Extraction** (`astral_chain/setup_game.py`, NSTool). The v1.0.1 update's ExeFS holds the game code: `main`,
   plus `rtld` and `sdk`, which are byte-identical to the base game's. Its RomFS is a patch over the base game's.
   NSTool applies it (`--basenca`), and `romfs_tool.c` packs the merged tree into one read-only image,
   `game/romfs.bin`. The game reads that image at run time.
2. **Patches.** Two pchtxt patches are applied to `main` before translation:
   - Dynamic-FPS: the measured frame time drives the game step, and the frame cap is 60;
   - `cutscene-catchup`: cutscenes may take larger steps after a stall, so they stay in sync with their audio.
3. **Translation** (`native_trace.py`, with LASRecomp's `lifter.py`). Every instruction of the executable segments
   is lifted from ARM64 to C, page by page. A page function `page_XXXX(Cpu *c)` dispatches on the guest PC. The
   play build uses three options:
   - `--lean`: no step counter or call trace;
   - `--direct-calls`: a `BL` calls its target page directly;
   - `--jumps`: branches within a page are `goto`s.
4. **Compilation** (`native_build.py`, Zig's clang). The 122 code units, the runtime (`runtime/host_runtime.c.in`
   and its headers) and the NVN layer (`runtime/las/`) are compiled at `-O2` for x86-64-v3 and linked into
   `build/game/round-00/trace.exe`. Objects are cached in `build/native-objects/`.

## At run time

- **CPU.** The original loader (`rtld`), SDK and game run as native code. Horizon kernel calls (SVCs) and system
  services are implemented in the runtime: memory, threads, synchronization, file system, saves, HID input, audio
  output, time, account and applets. Guest threads run on host threads in parallel (`AC_PARALLEL=1`, set by the
  launcher).
- **Graphics.** The game's NVN calls are intercepted at `nvnBootstrapLoader` and implemented by LASRecomp's NVN
  layer on Vulkan:
  - Maxwell shader binaries are translated to GLSL, then compiled to SPIR-V with glslang (cached in
    `build/game/cache/`);
  - textures are de-swizzled; ASTC, which desktop GPUs cannot sample, is decoded by a compute shader into RGBA8
    (`astc_gpu.c`, the same output as the CPU decoder `astc.c`, which remains the fallback);
  - pipelines are recorded as draws create them (`cache/pipelines.bin`) and built on idle worker threads ahead of
    their first draw in later sessions; a shipped list (`data/pipeline_variants.bin`) covers a first play;
  - with `VK_EXT_graphics_pipeline_library` (current NVIDIA and AMD drivers), every shader the game loads is compiled
    in the background into a shader library; a pipeline nobody recorded is then linked from those at its first draw
    without compiling, and its optimized version replaces it once built in the background;
  - before the game starts, the launcher runs the game's executable in a warm-up mode (`AC_GPU_WARM`) that builds all
    of that ahead of time for the GPU and driver in use: a shader library for every shader, and the recorded
    programs and pipelines. The driver keeps its Vulkan cache per executable name, so the game's own cache fills;
  - GPU memory written by the CPU is tracked with Windows write-watch.
- **Audio.** The game mixes its own audio. The runtime plays the PCM it outputs through Windows waveOut.
- **Saves.** They live in `userdata/saves/<title>/<profile>/`. The runtime exposes them to the game as its save
  file system.

## Safety

- `run_guarded.ps1` polls the Windows System log while the game runs. It kills the game on the first `nvlddmkm`,
  `amdkmdag` or `Display 4101` (TDR) event.
- The renderer stops the game if one GPU submission takes more than 1.2 s (`AC_GPU_HANG_MS`). That is before the
  Windows timeout that resets the driver.
