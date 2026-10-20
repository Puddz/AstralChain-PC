# Third-party notices

## Vendored in this repository

| Component | Where | Version | License |
| --- | --- | --- | --- |
| [LASRecomp](https://github.com/peiikai/LASRecomp) | `third_party/LASRecomp/` (`runtime/`, `tools/lifter.py`, `tools/simd_c.py`) | commit `a0b48fd4138b180726dc732fe6bff78ca796be81` | MIT, `third_party/LASRecomp/LICENSE` |
| LASRecomp NVN layer, modified | `astral_chain/runtime/las/` | same commit, changes listed in its `README.md` | MIT, `astral_chain/runtime/las/LICENSE-LASRecomp` |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `third_party/Vulkan-Headers/` (C headers only) | `vulkan-sdk-1.4.363.0` (`6802bb4733b63ed5efd3adb308a6c885ef180ea1`) | Apache-2.0 or MIT, `third_party/Vulkan-Headers/LICENSE.md` |
| [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | `third_party/VulkanMemoryAllocator/` | 3.1.0 | MIT, `third_party/VulkanMemoryAllocator/LICENSE.txt` |
| [AstralChain-Dynamic-FPS](https://github.com/yogesh1239/AstralChain-Dynamic-FPS) | `third_party/AstralChain-Dynamic-FPS/1.0.1.pchtxt` (`mods/Dynamic-60fps/exefs/1.0.1.pchtxt`) | v1.1.1, commit `79965b4df077f64b27b74456d0db88c6afac5bbe` | MIT, `third_party/AstralChain-Dynamic-FPS/LICENSE` |

The Maxwell shader translator is LASRecomp's `runtime/nvn/maxwell/translate.c` with reviewed changes applied at
build time by `astral_chain/shader_build.py`. The vendored file itself is unchanged.

## Downloaded by setup (not redistributed)

| Tool | Version | License |
| --- | --- | --- |
| [Zig](https://ziglang.org/) (C compiler) | 0.17.0 | MIT |
| [glslang](https://github.com/KhronosGroup/glslang) | 16.6.0 | BSD-3-Clause and others (see its archive) |
| [NSTool](https://github.com/jakcron/nstool) | 1.9.2 | MIT |

## Python packages (installed by setup)

[Capstone](https://www.capstone-engine.org/) (BSD-3-Clause), [python-lz4](https://github.com/python-lz4/python-lz4)
(BSD-3-Clause).
