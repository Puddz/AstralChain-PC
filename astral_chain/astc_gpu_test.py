"""Check the GPU ASTC decoder against the CPU one, byte for byte (development tool).

  python -m astral_chain.astc_gpu_test [astc_*.bin dumps...]

Builds runtime/astc_gpu_test.c with the project's Zig and runs it: random blocks for every 2D footprint, sRGB and
not, plus any texture dumps given (LARECOMP_DUMP_ASTC=<min width> with LARECOMP_GPU_ASTC=0 writes astc_*.bin into
the run folder). Needs a Vulkan GPU; no game data.
"""
from pathlib import Path
import subprocess
import sys

PROJECT = Path(__file__).resolve().parent.parent
RUNTIME = PROJECT/'astral_chain'/'runtime'
ZIG = PROJECT/'tools'/'zig'/'zig.exe'
GLSLANG = PROJECT/'tools'/'glslang'/'bin'/'glslang.exe'


def main():
    work = PROJECT/'build'/'astc-test'
    work.mkdir(parents=True, exist_ok=True)
    exe = work/'astc_gpu_test.exe'
    subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-windows-gnu', '-O2', '-I', str(RUNTIME), '-I', str(RUNTIME/'las'),
                    '-I', str(PROJECT/'third_party'/'Vulkan-Headers'/'include'), str(RUNTIME/'astc_gpu_test.c'),
                    str(RUNTIME/'las'/'nvn'/'astc.c'), '-o', str(exe)], check=True)
    sys.exit(subprocess.run([str(exe), str(GLSLANG), str(work), *sys.argv[1:]]).returncode)


if __name__ == '__main__':
    main()
