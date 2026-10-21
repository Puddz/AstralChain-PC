"""Optional setup step: precompute BC7 versions of the game's ASTC textures (the launcher's "Precompute BC7
textures"; runtime: las/nvn/vulkan/bc7_gpu.c, format: bc7_cache.h).

Every texture in the game's packs (WTA headers and WTP data: in .dat/.dtt, .evn and .eff packs, or loose, inside the root-level .pkz archives) is
decoded and encoded to BC7 on the CPU with bc7enc, on all threads. The game finds a texture by the hash of its bytes
as loaded; one it does not find is converted on the GPU instead.

  python -m astral_chain.bc7_cache [--quality 0-4] [--limit N] [--keys FILE]
"""
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import argparse
import ctypes
import hashlib
import os
import struct
import subprocess
import sys
import time

from . import shader_cache

PROJECT = Path(__file__).resolve().parent.parent
RUNTIME = PROJECT/'astral_chain'/'runtime'
BC7ENC = PROJECT/'third_party'/'bc7enc'
SOURCES = (RUNTIME/'bc7_convert.c', RUNTIME/'las'/'nvn'/'tegra.c', RUNTIME/'las'/'nvn'/'formats.c',
           RUNTIME/'las'/'nvn'/'astc.c', BC7ENC/'bc7enc.c')
HEADERS = (RUNTIME/'las'/'nvn'/'vulkan'/'bc7_cache.h', RUNTIME/'las'/'nvn'/'tegra.h', RUNTIME/'las'/'nvn'/'astc.h',
           BC7ENC/'bc7enc.h')
MAGIC, VERSION = 0x43374342, 1
_FOOTPRINTS = ((4, 4), (5, 4), (5, 5), (6, 5), (6, 6), (8, 5), (8, 6), (8, 8), (10, 5), (10, 6), (10, 8), (10, 10), (12, 10),
               (12, 12))
_ASTC_BLOCKS = {0x79 + i: fp for i, fp in enumerate(_FOOTPRINTS)} | {0x87 + i: fp for i, fp in enumerate(_FOOTPRINTS)}
DEFAULT_QUALITY = 1


def tool_digest() -> str:
    h = hashlib.sha256()
    for p in SOURCES + HEADERS:
        h.update(p.read_bytes())
    return h.hexdigest()[:16]


def build_tool(zig: Path, build: Path) -> Path:
    out = build/'bc7-convert'/tool_digest()
    dll = out/'bc7_convert.dll'
    if dll.is_file():
        return dll
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(zig), 'cc', '-target', 'x86_64-windows-gnu', '-O2', '-shared', '-I', str(RUNTIME),
                    '-I', str(RUNTIME/'las'), '-I', str(BC7ENC), *map(str, SOURCES), '-o', str(dll)], check=True)
    return dll


def _load(dll: Path):
    lib = ctypes.CDLL(str(dll))
    u32, u64, p = ctypes.c_uint32, ctypes.c_uint64, ctypes.c_void_p
    lib.bc7c_size.argtypes = [u32, u32, u32, u32, u32, u32]
    lib.bc7c_size.restype = u64
    lib.bc7c_key.argtypes = [p, u64, u32, u32, u32, u32, u32, u32, ctypes.POINTER(u64), ctypes.POINTER(u64)]
    lib.bc7c_key.restype = ctypes.c_int
    lib.bc7c_convert.argtypes = [p, u64, u32, u32, u32, u32, u32, u32, u32, p]
    lib.bc7c_convert.restype = ctypes.c_int
    lib.bc7c_init()
    return lib


def _inner(data):
    """Every file of a DAT pack, nested packs included."""
    try:
        items = list(shader_cache._dat_files(data))
    except Exception:
        return
    for name, blob in items:
        if blob[:4] == b'DAT\0':
            yield from _inner(blob)
        yield name, blob


def _textures(wta, wtp):
    """(XT1 fields, data, short level count or 0) of each texture of a WTA/WTP pair. Some textures are created with
    their mipmaps only down to one ASTC block (the same data, fewer levels): short is that level count."""
    if wta[:4] != b'WTB\0':
        return
    count, offsets, sizes, _, _, info = struct.unpack_from('<6I', wta, 8)
    for k in range(count):
        x = wta[info + k * 0x38:info + (k + 1) * 0x38]
        if x[:4] != b'XT1\0':
            continue
        off, = struct.unpack_from('<I', wta, offsets + 4 * k)
        size, = struct.unpack_from('<I', wta, sizes + 4 * k)
        levels, kind, fmt, w, h, depth, _, packaged = struct.unpack_from('<8I', x, 0x14)
        if kind not in (1, 8) or depth > 1 or not 0x79 <= fmt <= 0x94:
            continue
        bw, bh = _ASTC_BLOCKS[fmt]
        short = 1
        while max(w >> short, 1) >= bw and max(h >> short, 1) >= bh and short < levels:
            short += 1
        yield (fmt, w, h, levels, 6 if kind == 8 else 1, packaged), wtp[off:off + size], short if short < levels else 0


def _pairs(romfs: Path):
    """(texture fields, data) for every texture of the game, pack by pack."""
    decompress = shader_cache._zstd_decompress()
    with romfs.open('rb') as f:
        h = struct.unpack('<10Q', f.read(80))
        f.seek(h[7])
        table = f.read(h[8])
        packs, off = [], 0
        while off < len(table):
            _, _, doff, _, _, nlen = struct.unpack_from('<IIQQII', table, off)
            name = table[off + 32:off + 32 + nlen].decode('utf-8', 'replace')
            if name.endswith('.pkz'):
                packs.append(h[9] + doff)
            off += 32 + ((nlen + 3) & ~3)
        for base in packs:
            groups = {}
            for name, size, data_off, packed in shader_cache._pkz_entries(f, base):
                stem, ext = os.path.splitext(name)
                if ext in ('.dat', '.dtt', '.evn', '.eff', '.wta', '.wtp', '.wtb'):
                    groups.setdefault(stem, []).append((name, size, data_off, packed))
            for entries in groups.values():
                wta, wtp, wtb = {}, {}, []
                for entry, size, data_off, packed in entries:
                    f.seek(base + data_off)
                    raw = f.read(packed)
                    data = raw if packed == size else decompress(raw)
                    for name, blob in [(entry, data)] if data[:4] != b'DAT\0' else _inner(data):
                        stem, ext = os.path.splitext(name)
                        if ext == '.wta':
                            wta[stem] = blob
                        elif ext == '.wtp':
                            wtp[stem] = blob
                        elif ext == '.wtb':
                            wtb.append(blob)
                for stem, a in wta.items():
                    if stem in wtp:
                        yield from _textures(a, wtp[stem])
                for blob in wtb:
                    yield from _textures(blob, blob)  # (headers and data in one file)


def build(romfs: Path, out: Path, dll: Path, quality=DEFAULT_QUALITY, limit=0, keys_only=None, progress=print, only=None):
    """Writes out/index.bin and out/data.bin. Returns (textures, bytes, seconds)."""
    lib = _load(dll)
    t0 = time.time()

    def key(buf, fields):
        hsh, total = ctypes.c_uint64(), ctypes.c_uint64()
        if not lib.bc7c_key(buf, len(buf), *fields, ctypes.byref(hsh), ctypes.byref(total)):
            return None
        return hsh.value, total.value

    def convert(fields, buf):
        res = ctypes.create_string_buffer(lib.bc7c_size(*fields))
        return res.raw if lib.bc7c_convert(buf, len(buf), *fields, quality, res) else None

    seen, records, keys = set(), [], []
    offset = textures = 0
    out.mkdir(parents=True, exist_ok=True)
    threads = os.cpu_count() or 4
    pending = deque()  # (future, records to add: (hash, total, fields, size))
    with (out/'data.bin.partial').open('wb') as d, ThreadPoolExecutor(threads) as pool:
        def finish():
            nonlocal offset
            future, entries = pending.popleft()
            blocks = future.result()
            if blocks is None:
                return
            for hsh, total, fields, size in entries:  # (the short chain of a single layer: the start of the same data)
                records.append(struct.pack('<4Q6I', hsh, total, offset, size, *fields[:5], 0))
            d.write(blocks)
            offset += len(blocks)

        last = 0
        for fields, data, short in _pairs(romfs):
            buf = ctypes.create_string_buffer(data, len(data)) if data else None
            k = buf and key(buf, fields)
            if not k or (k, fields[:5]) in seen:
                continue
            if only is not None and k[0] not in only and not (short and (key(buf, fields[:3] + (short,) + fields[4:]) or
                                                                         (0,))[0] in only):
                continue
            seen.add((k, fields[:5]))
            entries = [(k[0], k[1], fields, lib.bc7c_size(*fields))]
            if short:
                sf = fields[:3] + (short,) + fields[4:]
                sk = key(buf, sf)
                if sk and (sk, sf[:5]) not in seen:
                    seen.add((sk, sf[:5]))
                    if fields[4] == 1:
                        entries.append((sk[0], sk[1], sf, lib.bc7c_size(*sf)))
                    elif not keys_only:  # (cube maps: levels per layer differ, a conversion of their own)
                        pending.append((pool.submit(convert, sf, buf), [(sk[0], sk[1], sf, lib.bc7c_size(*sf))]))
                    keys.append((sk, sf))
            keys.append((k, fields))
            textures += 1
            if not keys_only:
                pending.append((pool.submit(convert, fields, buf), entries))
                while len(pending) > threads * 4:
                    finish()
            if time.time() - last > 2:
                last = time.time()
                progress(f'  Converting textures to BC7: {textures} ({offset / 2**30:.1f} GB)')
            if limit and textures >= limit:
                break
        while pending:
            finish()
    if keys_only:
        with open(keys_only, 'w', encoding='ascii') as f:
            for (hsh, total), fields in keys:
                f.write(f'{hsh:016x} {total} {fields[0]:#x} {fields[1]}x{fields[2]} {fields[3]} {fields[4]}\n')
        (out/'data.bin.partial').unlink()
        return len(keys), 0, time.time() - t0
    records.sort(key=lambda r: struct.unpack_from('<Q', r)[0])  # (numeric order: the game's binary search)
    (out/'index.bin.partial').write_bytes(struct.pack('<3I', MAGIC, VERSION, len(records)) + b''.join(records))
    for name in ('data.bin', 'index.bin'):
        (out/(name + '.partial')).replace(out/name)
    return len(records), offset, time.time() - t0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--quality', type=int, default=DEFAULT_QUALITY)
    parser.add_argument('--limit', type=int, default=0)
    parser.add_argument('--keys', help='only write the texture keys to this file')
    parser.add_argument('--only', help='(tests) only the textures whose hashes this file lists, one per line')
    parser.add_argument('--out', default=str(PROJECT/'build'/'game'/'cache'/'bc7'))
    args = parser.parse_args()
    dll = build_tool(PROJECT/'tools'/'zig'/'zig.exe', PROJECT/'build')
    only = {int(x, 16) for x in open(args.only)} if args.only else None
    n, size, secs = build(PROJECT/'game'/'romfs.bin', Path(args.out), dll, args.quality, args.limit, args.keys,
                          lambda s: print(s, flush=True), only)
    print(f'{n} textures, {size / 2**30:.2f} GB, {secs:.0f} s')


if __name__ == '__main__':
    sys.exit(main())
