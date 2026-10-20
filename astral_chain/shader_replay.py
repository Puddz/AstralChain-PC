"""Replay captured shader translations offline with the current reviewed translator.

A run with LARECOMP_SAVE_MSL=1 leaves <addr>_<stage>.bin/.key in <run>/build/glsl. This retranslates every capture
with the translator as patched now (shader_build.py), compiles each result with the pinned glslang to SPIR-V and
reports translation and compilation failures. Nothing here touches the GPU.

  python -m astral_chain.shader_replay output/<run>/build/glsl --output output/<new folder>
"""
import argparse
import collections
import concurrent.futures
from pathlib import Path
import re
import subprocess

from .nvn_build import GLSLANG
from .shader_build import prepare_translator

PROJECT = Path(__file__).resolve().parent.parent
STAGES = {'vs': 'vert', 'fs': 'frag', 'cs': 'comp'}


def build_replay(reference: Path, compiler: Path, output: Path) -> Path:
    source = prepare_translator(reference, output/'translator')
    exe = output/'shader-replay.exe'
    subprocess.run([str(compiler), 'cc', '-O1', '-I', str(reference/'runtime/nvn/maxwell'),
                    '-I', str(Path(__file__).parent/'runtime/las'),
                    str(Path(__file__).parent/'runtime/shader_replay.c'), str(source), '-o', str(exe)], check=True)
    return exe


def replay_one(exe: Path, window: Path, output: Path):
    name = window.stem
    stage = name.rsplit('_', 1)[1]
    glsl = output/'glsl'/f'{name}.glsl'
    run = subprocess.run([str(exe), str(window), str(window.with_suffix('.key')), stage, str(glsl)],
                         capture_output=True, text=True, timeout=120)
    if run.returncode:
        return name, 'translate', (run.stderr or run.stdout).strip()
    spv = output/'spirv'/f'{name}.spv'
    comp = subprocess.run([str(GLSLANG), '-V', '--target-env', 'vulkan1.3', '-S', STAGES[stage], str(glsl), '-o', str(spv)],
                          capture_output=True, text=True, timeout=300)
    if comp.returncode:
        errors = [line for line in comp.stdout.splitlines() if 'ERROR' in line]
        return name, 'compile', '\n'.join(errors[:5]) or comp.stdout.strip()[-500:]
    return name, 'ok', ''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('captures', type=Path, nargs='+', help='build/glsl folders of LARECOMP_SAVE_MSL runs')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reference', type=Path, default=PROJECT/'third_party/LASRecomp')
    parser.add_argument('--compiler', type=Path, default=PROJECT/'tools/zig/zig.exe')
    parser.add_argument('--only', help='regular expression on <addr>_<stage>')
    args = parser.parse_args()
    output = args.output.resolve()
    for sub in ('glsl', 'spirv'):
        (output/sub).mkdir(parents=True, exist_ok=True)
    exe = build_replay(args.reference.resolve(), args.compiler.resolve(), output)
    windows = {}
    for folder in args.captures:
        for window in sorted(folder.glob('*.bin')):
            if window.with_suffix('.key').is_file() and (not args.only or re.search(args.only, window.stem)):
                windows.setdefault(window.stem, window)  # the first capture of an address wins
    results = collections.Counter()
    failures = []
    with concurrent.futures.ThreadPoolExecutor() as pool:
        for name, status, detail in pool.map(lambda w: replay_one(exe, w, output), windows.values()):
            results[status] += 1
            if status != 'ok':
                failures.append(f'{name} {status}: {detail}')
    report = '\n'.join(sorted(failures))
    (output/'failures.txt').write_text(report + '\n', encoding='utf-8')
    print(report)
    print(f'{len(windows)} shaders: ' + ', '.join(f'{k} {v}' for k, v in sorted(results.items())))


if __name__ == '__main__':
    main()
