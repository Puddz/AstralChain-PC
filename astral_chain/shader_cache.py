"""Shader precompilation before the game starts (setup and every launch).

The game's shaders are all in one archive of its RomFS, root.pkz (PlatinumGames PKZ: zstd-compressed DAT packs;
format research: github.com/cabalex/astral-extractor). They are extracted once to game/shaders as <name>.code /
<name>.control pairs, the two halves the game hands to nvnProgramSetShaders.

Then shader_precompile (runtime/shader_precompile.c) translates every one of them and compiles the SPIR-V missing
from build/game/cache/spirv, on all cores: about 20 s the first time, nothing afterwards until something changes.
It also covers the recorded variants: the list shipped in astral_chain/data/shader_variants.bin (hashes of shader
code and the draw state they were used with, no game data) and the ones recorded on this PC
(build/game/cache/programs.bin), for the few draws whose shaders differ from the generic translation.

The shipped lists (shader_variants.bin, and pipeline_variants.bin: the render state of the pipelines draws created)
are also merged into this PC's cache/programs.bin and cache/pipelines.bin, which the game reads to build programs
and pipelines ahead of their first draw, so a first play starts with them too.
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
SHIPPED_PIPELINES = PROJECT/'astral_chain'/'data'/'pipeline_variants.bin'
# Record files: a 4-byte magic (base ^ record size, exec.c WARM_MAGIC / PREC_MAGIC), then fixed-size records
RECORD_FILES = ((SHIPPED_VARIANTS, 'programs.bin', 0x4D524157), (SHIPPED_PIPELINES, 'pipelines.bin', 0x45504950))
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
        return lambda data: zstandard.ZstdDecompressor().decompressobj().decompress(data)
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
    if decompress is None:  # (Python before 3.14, set up before requirements.txt listed it)
        subprocess.run([sys.executable, '-m', 'pip', 'install', '--disable-pip-version-check', '-q', 'zstandard==0.25.0'])
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


def tool_digest() -> str:
    """Identity of shader_precompile.exe: its sources and the translator patches."""
    digest = hashlib.sha256()
    for path in TOOL_SOURCES + (PROJECT/'astral_chain'/'shader_build.py', RUNTIME/'las'/'las_compat.h',
                                LASRECOMP/'runtime'/'nvn'/'maxwell'/'translate.c'):
        digest.update(path.read_bytes())
    return digest.hexdigest()[:16]


def build_tool(zig: Path, build: Path) -> Path:
    """shader_precompile.exe, rebuilt when its sources or the translator patches change."""
    folder = build/'shader-precompile'/tool_digest()
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


def _precompile_state(tool_id: str, game_build: Path, sizes=None) -> str:
    sizes = sizes or {}
    records = [p for p in (SHIPPED_VARIANTS, game_build/'cache'/'programs.bin') if p.is_file() or p.name in sizes]
    return ' '.join([tool_id] + [f'{sizes.get(p.name) or p.stat().st_size}' for p in records])


def precompile(tool: Path, shaders: Path, game_build: Path, glslang: Path) -> None:
    records = [str(p) for p in (SHIPPED_VARIANTS, game_build/'cache'/'programs.bin') if p.is_file()]
    # Nothing new since the last complete run (same tool, same records): skip the 2-second check of the cache
    stamp = game_build/'cache'/'spirv'/'precompiled.txt'
    state = _precompile_state(tool.parent.name, game_build)
    if stamp.is_file() and stamp.read_text(encoding='ascii') == state:
        return
    for leftover in (game_build/'cache'/'spirv').glob('*.spv.*'):  # (stores whose rename failed, before spirv_cache.c retried)
        leftover.unlink(missing_ok=True)
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
            if not failed:
                stamp.write_text(state, encoding='ascii')
    process.wait()


def _records(path: Path, base: int):
    """(magic, set of records) of a record file, or (None, None) if it is missing or not that kind of file."""
    if not path.is_file():
        return None, None
    data = path.read_bytes()
    if len(data) < 4:
        return None, None
    magic = struct.unpack_from('<I', data)[0]
    size = magic ^ base
    if not 0 < size < 65536:
        return None, None
    body = data[4:len(data) - (len(data) - 4) % size]
    return magic, [body[i:i + size] for i in range(0, len(body), size)]


def merge_records(game_build: Path, dry_run=False) -> dict:
    """Append the shipped records this PC's cache lacks. A cache file of another layout is left to the game, which
    starts it afresh (and the next launch merges into the new one). Returns the new sizes of the files merged into
    (dry_run: the sizes they would get; nothing is written)."""
    cache = game_build/'cache'
    sizes = {}
    for shipped, name, base in RECORD_FILES:
        magic, records = _records(shipped, base)
        if not records:
            continue
        target = cache/name
        have_magic, have = _records(target, base)
        if target.is_file() and have_magic is None and target.stat().st_size >= 4:
            continue  # (unknown content: not touched)
        if have_magic is not None and have_magic != magic:
            continue
        known = set(have or ())
        new = [r for r in records if r not in known]
        if not new:
            continue
        sizes[name] = (target.stat().st_size if have_magic is not None else 4) + sum(map(len, new))
        if dry_run:
            continue
        cache.mkdir(parents=True, exist_ok=True)
        with target.open('ab') as f:
            if have_magic is None:
                f.truncate(0)
                f.write(struct.pack('<I', magic))
            f.write(b''.join(new))
    return sizes


def _warm_state(game_build: Path, sizes=None) -> str:
    """The build and record part of the warm-up's state (exec.c adds the GPU, driver and shader library use)."""
    sizes = sizes or {}
    fingerprint = game_build/'build-fingerprint.txt'
    parts = [fingerprint.read_text(encoding='ascii').strip() if fingerprint.is_file() else '']
    for name in ('programs.bin', 'pipelines.bin'):
        path = game_build/'cache'/name
        parts.append(f'{sizes.get(name) or (path.stat().st_size if path.is_file() else 0)}')
    if os.environ.get('LARECOMP_GPL') == '0':
        parts.append('no-gpl')  # (asked for; the GPU's own support is in the part exec.c adds)
    return ' '.join(parts)


def status(game_build: Path) -> str:
    """'compiled' when a launch would neither translate nor compile shaders (as far as can be told without the GPU:
    a new driver is noticed by the warm-up itself), 'needed' when it would, 'never' before the first time."""
    stamp = game_build/'cache'/'spirv'/'precompiled.txt'
    warm = game_build/'cache'/'gpu_warm.txt'
    if not stamp.is_file() and not warm.is_file():
        return 'never'
    sizes = merge_records(game_build, dry_run=True)
    if not stamp.is_file() or stamp.read_text(encoding='ascii') != _precompile_state(tool_digest(), game_build, sizes):
        return 'needed'
    if os.environ.get('AC_GPU_WARM') != '0':
        done = warm.read_text(encoding='utf-8', errors='replace') if warm.is_file() else ''
        if not done.startswith(_warm_state(game_build, sizes) + ' | '):
            return 'needed'
    return 'compiled'


def forget(game_build: Path) -> None:
    """The next prepare checks every shader and compiles for the GPU again (the caches themselves are kept)."""
    for stamp in (game_build/'cache'/'spirv'/'precompiled.txt', game_build/'cache'/'gpu_warm.txt'):
        stamp.unlink(missing_ok=True)


def warm_gpu(game_build: Path, shaders: Path, glslang: Path) -> None:
    """Compile for this GPU and driver before the game (the game's executable in its AC_GPU_WARM mode, exec.c
    vk_gpu_warm): shader libraries for every shader, the recorded programs and pipelines, into the driver's cache and
    cache/vk_pipelines.bin. It skips itself when nothing changed (game build, records, GPU, driver)."""
    exe = game_build/'round-00'/'trace.exe'
    if not exe.is_file():
        return
    env = dict(os.environ, AC_GPU_WARM=str(shaders.resolve()), AC_GPU_WARM_STATE=_warm_state(game_build),
               AC_GLSLANG=str(glslang))
    process = subprocess.Popen([str(exe)], cwd=game_build, env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                               text=True, errors='replace')
    shown = False
    try:
        for line in process.stdout:
            words = line.split()
            if words[:1] == ['progress']:
                print(f'\r  Compiling for your GPU: {words[1]}/{words[2]}', end='', flush=True)
                shown = True
            elif words[:1] == ['done']:
                if shown:
                    print()
                if words[1:2] != ['skipped']:
                    print('  GPU ready: ' + ' '.join(words[1:]))
        process.wait(timeout=900)
    except subprocess.TimeoutExpired:
        process.kill()
        print('\n  GPU warm-up stopped (took too long); the game compiles in play instead.', file=sys.stderr)


def prepare(romfs: Path, game: Path, game_build: Path, zig: Path, glslang: Path, build: Path) -> None:
    """Extract (once), build the tool (when changed) and fill the cache. Never stops the game from starting."""
    try:
        merge_records(game_build)
    except Exception as error:
        print(f'  Recorded shader variants not merged: {error}', file=sys.stderr)
    try:
        shaders = game/'shaders'
        if not extract_shaders(romfs, shaders):
            return
        precompile(build_tool(zig, build), shaders, game_build, glslang)
    except Exception as error:  # (the game still runs; it compiles shaders in play as before)
        print(f'  Shader precompilation skipped: {error}', file=sys.stderr)
        return
    if os.environ.get('AC_GPU_WARM') == '0':
        return
    try:
        warm_gpu(game_build, game/'shaders', glslang)
    except Exception as error:
        print(f'  GPU warm-up skipped: {error}', file=sys.stderr)
