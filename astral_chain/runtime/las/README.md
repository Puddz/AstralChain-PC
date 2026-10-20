# Vendored LASRecomp NVN layer

Source: LASRecomp `runtime/nvn` at commit `a0b48fd4138b180726dc732fe6bff78ca796be81`
(MIT, see `LICENSE-LASRecomp`). LASRecomp implements Nintendo's NVN graphics API on
the host for *Link's Awakening*; Astral Chain resolves 513 NVN names through
`nvnBootstrapLoader`, and runtime `nvn_hle.h` binds them to these implementations
when `native_trace --nvn-hle --nvn-backend ...` is used.

Local shims replace LASRecomp's runtime headers: `hle.h`, `rt.h`, `platform.h`,
`kernel.h` (implemented by `../nvn_hle.h`). `null_backend.c` is a diagnostic backend
that executes and draws nothing.

Astral Chain changes to vendored files (keep this list current):

- `nvn/renderer.h`: SDL input hooks removed.
- `nvn/objects.c`: `device_integer` reports only values measured from this game's
  SDK NVN (API 53.310); unmeasured indices stop with `FATAL`.
- `nvn/vulkan/exec.c`: `CMD_BIND_PROGRAM` honours NVNshaderStageBits (the game binds
  separate vertex and fragment programs); LASRecomp's Link's Awakening GPU-timer
  pacing is off unless `LARECOMP_PACE=1`.
- `nvn/vulkan/device.c`: Win32 window from the runtime (`nvn_host_window`) instead
  of SDL; letterboxed presentation at render scale 1; uncompressed PNG writer;
  no UI thread, menu, widescreen or test hooks; screenshots in the run directory.

Vulkan backend support files: `shim/` (SDL timer/CPU count, shaderc API),
`shaderc_glslang.c` (runs the pinned glslang), `game_hooks_neutral.c` (Link's
Awakening hooks report inactive), `settings.h`, `texpatch.h`, `las_compat.h`,
`vma.cpp` (VulkanMemoryAllocator v3.1.0 under `third_party/`). The Maxwell
translator is the reviewed derivative from `astral_chain/shader_build.py`.

Not vendored: Metal backend, texture patches (`texpatch`), menu/imgui, SMAA and
other Link's Awakening specific features.
