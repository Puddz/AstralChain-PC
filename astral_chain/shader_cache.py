"""Shader precompilation before the game starts (setup and every launch).

The game's shaders are all in one archive of its RomFS, root.pkz (PlatinumGames PKZ: zstd-compressed DAT packs;
format research: github.com/cabalex/astral-extractor). They are extracted once to game/shaders as <name>.code /
<name>.control pairs, the two halves the game hands to nvnProgramSetShaders.

At each launch, shader_precompile (runtime/shader_precompile.c) translates them for every known draw-time variant
and compiles the SPIR-V that is missing from build/game/cache/spirv, on all cores. Known variants: the list shipped
in astral_chain/data/shader_variants.bin (hashes of shader code and the draw state they were used with, no game
data) and the ones recorded on this PC (build/game/cache/programs.bin). Shaders met for the first time in play are
still compiled then, and recorded for the next launch.
"""
from pathlib import Path
import hashlib
import os
import struct
import subprocess
import sys

from .shader_build import prepare_translator

PROJECT = Path(__file__).resolve().parent.parent
RUNTIME = PROJECT/'astral_chain'/'runtime'
LASRECOMP = PROJECT/'third_party'/'LASRecomp'
SHIPPED_VARIANTS = PROJECT/'astral_chain'/'data'/'shader_variants.bin'
SHADER_DATS = ('autoshadereff.dat', 'shader.dat', 'shader2.dat', 'shadereff.dat', 'shadereffcs.dat',
               'shaderefflayout.dat')
TOOL_SOURCES = (RUNTIME/'shader_precompile.c', RUNTIME/'las'/'nvn'/'vulkan'/'spirv_cache.c',
                RUNTIME/'las'/'shaderc_glslang.c')


def _zstd_decompress():
    try:
        from compression import zstd  # Python 3.14+
        return zstd.decompress
    except ImportError:
        pass
    try:
        import zstandard
        return lambda data: zstandard.ZstdDecompressor().decompress(data)
    except ImportError:
        return None


def _romfs_file(romfs: Path, wanted: str):
    """(offset, size) of a root-level file in a Horizon RomFS image."""
    with romfs.open('rb') as f:
        h = struct.unpack('<10Q', f.read(80))
        f.seek(h[7])
        files = f.read(h[8])
    off = 0
    while off < len(files):
        parent, _, doff, size, _, nlen = struct.unpack_from('<IIQQII', files, off)
        name = files[off + 32:off + 32 + nlen].decode('utf-8', 'replace')
        if parent == 0 and name == wanted:
            return h[9] + doff, size
        off += 32 + ((nlen + 3) & ~3)
    raise FileNotFoundError(wanted)


def _pkz_entries(f, base):
    f.seek(base)
    magic, _, _, count, table, names_size = struct.unpack('<4sIQIIQ', f.read(0x20))
    if magic != b'pkzl':
        raise ValueError('not a PKZ archive')
    f.seek(base + table)
    entries = f.read(count * 0x20)
    names = f.read(names_size)
    for i in range(count):
        name_off, size, data_off, packed = struct.unpack_from('<QQQQ', entries, i * 0x20)
        yield names[name_off:names.index(b'\0', name_off)].decode('ascii'), size, data_off, packed


def _dat_files(data):
    if data[:4] != b'DAT\0':
        raise ValueError('not a DAT pack')
    count, files, _, names, sizes, _ = struct.unpack_from('<6I', data, 4)
    width = struct.unpack_from('<I', data, names)[0]
    for i in range(count):
        offset = struct.unpack_from('<I', data, files + 4 * i)[0]
        size = struct.unpack_from('<I', data, sizes + 4 * i)[0]
        name = data[names + 4 + i * width:names + 4 + (i + 1) * width].split(b'\0')[0].decode('ascii', 'replace')
        yield name, data[offset:offset + size]


def extract_shaders(romfs: Path, out: Path) -> int:
    """Split every shader of root.pkz into out/<dat>/<name>.code and .control. Returns the count (0: not possible)."""
    done = out/'complete.txt'
    if done.is_file():
        return int(done.read_text(encoding='ascii').split()[0])
    decompress = _zstd_decompress()
    if decompress is None:
        print('  (shader precompilation needs Python 3.14, or the "zstandard" package: skipped)')
        return 0
    base, _ = _romfs_file(romfs, 'root.pkz')
    count = 0
    with romfs.open('rb') as f:
        for name, size, data_off, packed in list(_pkz_entries(f, base)):
            if name not in SHADER_DATS:
                continue
            f.seek(base + data_off)
            raw = f.read(packed)
            data = raw if packed == size else decompress(raw)
            folder = out/name[:-4]
            folder.mkdir(parents=True, exist_ok=True)
            for inner, blob in _dat_files(data):
                _, _, control_size, control_off, code_size, code_off = struct.unpack_from('<4sIIIII', blob)
                (folder/(inner + '.control')).write_bytes(blob[control_off:control_off + control_size])
                (folder/(inner + '.code')).write_bytes(blob[code_off:code_off + code_size])
                count += 1
    done.write_text(f'{count}\n', encoding='ascii')
    return count


def build_tool(zig: Path, build: Path) -> Path:
    """shader_precompile.exe, rebuilt when its sources or the translator patches change."""
    digest = hashlib.sha256()
    for path in TOOL_SOURCES + (PROJECT/'astral_chain'/'shader_build.py', RUNTIME/'las'/'las_compat.h',
                                LASRECOMP/'runtime'/'nvn'/'maxwell'/'translate.c'):
        digest.update(path.read_bytes())
    folder = build/'shader-precompile'/digest.hexdigest()[:16]
    exe = folder/'shader_precompile.exe'
    if exe.is_file():
        return exe
    translator = prepare_translator(LASRECOMP, folder/'translator')
    las = RUNTIME/'las'
    command = [str(zig), 'cc', '-target', 'x86_64-windows-gnu', '-O2', '-include', str(las/'las_compat.h'),
               '-I', str(las), '-I', str(las/'shim'), '-I', str(LASRECOMP/'runtime'),
               '-I', str(LASRECOMP/'runtime'/'nvn'/'maxwell'), *map(str, TOOL_SOURCES), str(translator),
               '-Wl,--stack,67108864', '-o', str(exe)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError('shader precompiler build failed:\n' + result.stdout + result.stderr)
    return exe


def precompile(tool: Path, shaders: Path, game_build: Path, glslang: Path) -> None:
    records = [str(p) for p in (SHIPPED_VARIANTS, game_build/'cache'/'programs.bin') if p.is_file()]
    threads = max(1, (os.cpu_count() or 4) - 2)
    env = dict(os.environ, AC_GLSLANG=str(glslang))
    process = subprocess.Popen([str(tool), str(shaders), str(threads), *records], cwd=game_build, env=env,
                               stdout=subprocess.PIPE, text=True)
    total = 0
    for line in process.stdout:
        words = line.split()
        if words[:1] == ['progress']:
            done, total = int(words[1]), int(words[2])
            print(f'\r  Preparing shaders: {done}/{total}', end='', flush=True)
        elif words[:1] == ['done']:
            if total:
                print()
            variants, unknown, compiled, cached, failed = map(int, words[1:])
            print(f'  Shaders ready: {compiled} compiled now, {cached} already cached'
                  + (f', {failed} failed' if failed else ''))
    process.wait()


def prepare(romfs: Path, game: Path, game_build: Path, zig: Path, glslang: Path, build: Path) -> None:
    """Extract (once), build the tool (when changed) and fill the cache. Never stops the game from starting."""
    try:
        shaders = game/'shaders'
        if not extract_shaders(romfs, shaders):
            return
        precompile(build_tool(zig, build), shaders, game_build, glslang)
    except Exception as error:  # (the game still runs; it compiles shaders in play as before)
        print(f'  Shader precompilation skipped: {error}', file=sys.stderr)
