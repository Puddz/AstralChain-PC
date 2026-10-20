"""Bounded raw RomFS identity/header validation; does not decrypt archives."""
from pathlib import Path
import hashlib
import os
import struct
from .containers import FormatError


def inspect_romfs(path):
    path = Path(path).resolve()
    with path.open('rb') as source:
        before = os.fstat(source.fileno())
        if not 80 <= before.st_size <= 64*1024**3:
            raise FormatError('RomFS image size outside budget')
        header = source.read(80)
        fields = struct.unpack('<10Q', header)
        if fields[0] != 80 or not 80 <= fields[9] <= before.st_size:
            raise FormatError('Invalid RomFS header/data offset')
        spans = [(0, 80)]
        for i in (1, 3, 5, 7):
            offset, size = fields[i:i+2]
            if size > 64*1024**2 or offset > before.st_size or size > before.st_size-offset:
                raise FormatError('RomFS table outside image/budget')
            if (i in (1, 5) and (size < 4 or size % 4)) or (i == 3 and size < 24):
                raise FormatError('Invalid RomFS table size')
            spans.append((offset, offset+size))
        spans.sort()
        if any(a[1] > b[0] for a, b in zip(spans, spans[1:])):
            raise FormatError('Overlapping RomFS tables')
        if any(start <= fields[9] < end for start,end in spans):
            raise FormatError('RomFS data starts in metadata')
        source.seek(fields[3])
        root = source.read(24)
        if len(root) != 24 or struct.unpack_from('<I',root,20)[0] != 0:
            raise FormatError('Invalid RomFS root entry')
        source.seek(0)
        digest = hashlib.file_digest(source,'sha256').hexdigest()
        after = os.fstat(source.fileno())
        if (before.st_size,before.st_mtime_ns) != (after.st_size,after.st_mtime_ns):
            raise FormatError('RomFS changed during identity check')
    return {'path':str(path),'size':before.st_size,'sha256':digest,'header':header.hex(),
            'data_offset':fields[9],'scope':'Header bounds and whole-file identity; not an integrity-tree verification'}


def runtime_constants(info):
    if info is None:
        return ''
    units = struct.unpack('<%dH' % (len(info['path'].encode('utf-16le'))//2),info['path'].encode('utf-16le'))
    return ('#define ROMFS_CONFIGURED 1\n'
            f'#define ROMFS_EXPECTED_SIZE {info["size"]}ull\n'
            'static const wchar_t romfs_path[]={'+','.join(map(str,units))+',0};\n'
            'static const unsigned char romfs_expected_header[]={'+','.join(str(x) for x in bytes.fromhex(info['header']))+'};\n')
