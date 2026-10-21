"""Measure the GPU BC7 encoder (LARECOMP_BC7=1) on test images and ASTC dumps (development tool).

  python -m astral_chain.bc7_gpu_test [astc_*.bin dumps...]

Builds runtime/bc7_gpu_test.c with the project's Zig and runs it: every block must be BC7 mode 6, and smooth test
images must keep at least 40 dB PSNR. Dumps: LARECOMP_DUMP_ASTC=<min width> (see astc_gpu_test). Needs a Vulkan
GPU; no game data.
"""
from pathlib import Path
import subprocess
import sys

PROJECT = Path(__file__).resolve().parent.parent
RUNTIME = PROJECT/'astral_chain'/'runtime'
ZIG = PROJECT/'tools'/'zig'/'zig.exe'
GLSLANG = PROJECT/'tools'/'glslang'/'bin'/'glslang.exe'


def main():
    work = PROJECT/'build'/'bc7-test'
    work.mkdir(parents=True, exist_ok=True)
    exe = work/'bc7_gpu_test.exe'
    subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-windows-gnu', '-O2', '-I', str(RUNTIME), '-I', str(RUNTIME/'las'),
                    '-I', str(PROJECT/'third_party'/'Vulkan-Headers'/'include'), str(RUNTIME/'bc7_gpu_test.c'),
                    str(RUNTIME/'las'/'nvn'/'astc.c'), '-o', str(exe)], check=True)
    sys.exit(subprocess.run([str(exe), str(GLSLANG), str(work), *sys.argv[1:]]).returncode)


if __name__ == '__main__':
    main()
