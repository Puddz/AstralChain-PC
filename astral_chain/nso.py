"""Bounded NSO/MOD0 inspection. Images and generated code stay in ignored output."""
from pathlib import Path
import hashlib
import json
import struct
from collections import Counter
from .containers import FormatError

MAX_IMAGE = 512 * 1024 * 1024


class Nso:
    def __init__(self, path):
        self.path = Path(path)
        if not 256 <= self.path.stat().st_size <= MAX_IMAGE:
            raise FormatError("NSO file size outside supported limits")
        raw = self.path.read_bytes()
        if raw[:4] != b"NSO0" or struct.unpack_from("<I", raw, 4)[0] != 0:
            raise FormatError("Unsupported NSO magic/version")
        self.sha256 = hashlib.sha256(raw).hexdigest()
        self.build_id = raw[0x40:0x60].hex()
        flags = struct.unpack_from("<I", raw, 12)[0]
        if flags & ~63:
            raise FormatError("Unsupported NSO flags")
        segments, ranges, file_ranges = [], [], []
        self.segment_hashes = []
        for i in range(3):
            file_offset, address, length = struct.unpack_from("<3I", raw, 16 + 16*i)
            stored = struct.unpack_from("<I", raw, 96 + 4*i)[0] if flags & (1 << i) else length
            if length > MAX_IMAGE or address + length > MAX_IMAGE or file_offset < 256 or file_offset + stored > len(raw):
                raise FormatError("NSO segment extent outside file/image")
            for low, high in ranges:
                if address < high and address + length > low:
                    raise FormatError("Overlapping NSO memory segments")
            for low, high in file_ranges:
                if file_offset < high and file_offset + stored > low:
                    raise FormatError("Overlapping NSO file segments")
            data = raw[file_offset:file_offset+stored]
            if flags & (1 << i):
                import lz4.block
                try:
                    data = lz4.block.decompress(data, uncompressed_size=length)
                except lz4.block.LZ4BlockError as exc:
                    raise FormatError("Invalid NSO LZ4 segment") from exc
            if len(data) != length:
                raise FormatError("NSO segment decompressed size mismatch")
            digest = hashlib.sha256(data).digest()
            checked = bool(flags & (8 << i))
            if checked and digest != raw[160+32*i:192+32*i]:
                raise FormatError("NSO segment hash mismatch")
            self.segment_hashes.append({"sha256": digest.hex(), "header_verified": checked})
            segments.append((address, data))
            ranges.append((address, address+length))
            file_ranges.append((file_offset, file_offset+stored))
        self.text, self.rodata, self.data = [(a, len(b)) for a, b in segments]
        self.bss_size = struct.unpack_from("<I", raw, 60)[0]
        end = self.data[0] + self.data[1] + self.bss_size
        if end > MAX_IMAGE or end < max(high for _, high in ranges):
            raise FormatError("Invalid NSO image/BSS extent")
        self.size = (end + 4095) & ~4095
        self.image = bytearray(self.size)
        for address, data in segments:
            self.image[address:address+len(data)] = data
        self.parse_mod0()

    def unpack(self, fmt, offset):
        size = struct.calcsize(fmt)
        if offset < 0 or offset + size > len(self.image):
            raise FormatError(f"NSO memory read outside image at {offset:#x}")
        return struct.unpack_from(fmt, self.image, offset)

    def u32(self, offset):
        return self.unpack("<I", offset)[0]

    def u64(self, offset):
        return self.unpack("<Q", offset)[0]

    def cstr(self, offset):
        if not self.strtab <= offset < self.strtab + self.strsize:
            raise FormatError("NSO string offset outside dynamic string table")
        end = self.image.find(0, offset, min(self.strtab + self.strsize, offset + 16384))
        if end == -1:
            raise FormatError("Unterminated NSO dynamic string")
        try:
            return self.image[offset:end].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise FormatError("Invalid NSO dynamic string encoding") from exc

    def parse_mod0(self):
        mod0 = self.u32(4)
        magic, *relative = self.unpack("<4s6i", mod0)
        if magic != b"MOD0":
            raise FormatError("Missing NSO MOD0 header")
        self.dynamic, self.bss_start, self.bss_end, self.eh_start, self.eh_end, self.module_obj = [mod0 + x for x in relative]
        if not 0 <= self.bss_start <= self.bss_end <= self.size or not 0 <= self.eh_start <= self.eh_end <= self.size:
            raise FormatError("Invalid MOD0 BSS/unwind extents")
        tags, needed = {}, []
        for index in range(65536):
            tag, value = self.unpack("<qQ", self.dynamic + index*16)
            if tag == 0:
                break
            if tag == 1:
                needed.append(value)
            elif tag in tags:
                raise FormatError("Duplicate NSO dynamic tag")
            tags[tag] = value
        else:
            raise FormatError("NSO dynamic table exceeds limit or lacks terminator")
        self.dyn = tags
        self.strtab, self.strsize = tags.get(5, 0), tags.get(10, 0)
        if self.strtab + self.strsize > self.size:
            raise FormatError("NSO dynamic string table outside image")
        self.needed = [self.cstr(self.strtab + n) for n in needed]
        self.syms = []
        self.symbol_visibility = []
        if 6 in tags:
            if 4 not in tags or tags.get(11, 24) != 24:
                raise FormatError("Unsupported NSO symbol table layout")
            count = self.u32(tags[4] + 4)
            if count > 1_000_000 or tags[6] + count*24 > self.size:
                raise FormatError("Invalid NSO symbol count/extent")
            for i in range(count):
                name, info, other, section, value, size = self.unpack("<IBBHQQ", tags[6]+i*24)
                self.syms.append((self.cstr(self.strtab + name), info, section, value, size))
                if other & ~3:
                    raise FormatError('Unsupported NSO symbol visibility flags')
                self.symbol_visibility.append(other)
        self.relocs = []
        for base_tag, size_tag in ((7, 8), (23, 2)):
            if base_tag not in tags:
                continue
            base, size = tags[base_tag], tags.get(size_tag, 0)
            if size % 24 or size > 24_000_000 or base + size > self.size or tags.get(9, 24) != 24:
                raise FormatError("Invalid NSO relocation table")
            for i in range(size//24):
                address, info, addend = self.unpack("<QQq", base+24*i)
                symbol, kind = info >> 32, info & 0xffffffff
                if address + 8 > self.size or symbol >= len(self.syms):
                    raise FormatError("NSO relocation target/symbol outside image")
                self.relocs.append((address, kind, symbol, addend))

    def imports(self):
        return [s for s in self.syms if s[0] and s[2] == 0]

    def exports(self):
        return [s for s in self.syms if s[0] and s[2] != 0]

    def report(self):
        return {"module": self.path.name, "sha256": self.sha256, "build_id": self.build_id,
                "image_size": self.size, "text": self.text, "rodata": self.rodata, "data": self.data,
                "bss_size": self.bss_size, "segment_hashes": self.segment_hashes,
                "needed": self.needed, "import_count": len(self.imports()), "export_count": len(self.exports()),
                "relocations": dict(Counter(r[1] for r in self.relocs)), "imports": self.imports(), "exports": self.exports()}


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exefs", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    reports = [Nso(args.exefs / name).report() for name in ("rtld", "main", "sdk")]
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(reports, output, indent=2)
        output.write("\n")
    for report in reports:
        print(report["module"], report["build_id"], report["import_count"], "imports", report["export_count"], "exports")


if __name__ == "__main__":
    main()
