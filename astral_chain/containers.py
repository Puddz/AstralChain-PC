"""Bounded readers for the public XCI/HFS0/PFS0 container structures.

Offsets in Node are absolute within the backing file. No decryption, signature
verification, or interpretation of NCA payloads is performed.
"""
from dataclasses import dataclass
from pathlib import Path
import struct


class FormatError(ValueError):
    """Malformed input or an explicitly unsupported resource requirement."""


@dataclass(frozen=True)
class Node:
    source: Path
    name: str
    offset: int
    size: int
    kind: str
    expanded_size: int | None = None
    origin: str | None = None

    def read(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > self.size:
            raise FormatError(f"Read outside {self.name}: offset={offset}, size={size}")
        with self.source.open("rb") as stream:
            stream.seek(self.offset + offset)
            data = stream.read(size)
        if len(data) != size:
            raise FormatError(f"Truncated backing file: {self.source}")
        return data

    @property
    def browsable(self) -> bool:
        return self.kind in {"directory", "XCI", "HFS0", "PFS0", "PKZ", "DAT"}


def identify(node: Node) -> str:
    magic = node.read(0, min(4, node.size))
    if magic in (b"PFS0", b"HFS0"):
        return magic.decode("ascii")
    if magic == b"pkzl":
        return "PKZ"
    if node.size >= 0x140 and node.read(0x100, 4) == b"HEAD":
        return "XCI"
    # These labels identify signatures or filename hints, not validated assets.
    if magic in (b"WMB3", b"WMB4", b"NSO0", b"DAT\0", b"DDS "):
        return magic.decode("ascii").rstrip("\0 ")
    suffix = Path(node.name).suffix.lower()
    if suffix == ".nca":
        return "NCA (opaque)"
    return suffix[1:].upper() or "binary"


def open_path(path: Path) -> Node:
    path = path.resolve(strict=True)
    if path.is_dir():
        return Node(path, path.name, 0, 0, "directory")
    node = Node(path, path.name, 0, path.stat().st_size, "unknown")
    return Node(path, node.name, 0, node.size, identify(node))


def children(node: Node) -> list[Node]:
    if node.kind == "directory":
        # Do not follow directory links/junctions back into ancestors.
        return [open_path(p) for p in sorted(node.source.iterdir(),
                key=lambda p: (not p.is_dir(), p.name.casefold()))
                if not p.is_symlink() and not p.is_junction()]
    if node.kind == "XCI":
        offset = struct.unpack("<Q", node.read(0x130, 8))[0]
        if offset < 0x140 or offset + 16 > node.size:
            raise FormatError("XCI root table lies outside the image")
        table = Node(node.source, node.name, node.offset + offset,
                     node.size - offset, "HFS0")
        return read_table(table)
    if node.kind in {"HFS0", "PFS0"}:
        return read_table(node)
    if node.kind == "PKZ":
        return read_pkz(node)
    if node.kind == "DAT":
        return read_dat(node)
    return []


def read_dat(node: Node) -> list[Node]:
    """DAT and DTT share DAT\0 magic and fixed-width UTF-8 name slots."""
    magic, count, offsets, extensions, names, sizes, hashes = struct.unpack(
        "<4s6I", node.read(0, 28))
    if magic != b"DAT\0":
        raise FormatError("Invalid DAT/DTT signature")
    if count > 100_000:
        raise FormatError("DAT entry count exceeds inspector limit")
    if not count:
        return []
    for offset in (offsets, extensions, names, sizes):
        if offset < 28:
            raise FormatError("DAT metadata overlaps header")
    offset_table = node.read(offsets, count * 4)
    node.read(extensions, count * 4)
    size_table = node.read(sizes, count * 4)
    stride = struct.unpack("<I", node.read(names, 4))[0]
    if not 1 <= stride <= 4096 or stride * count > 64 * 1024 * 1024:
        raise FormatError("DAT name table exceeds inspector limit")
    name_table = node.read(names + 4, stride * count)
    metadata_end = max(28, offsets + count * 4, extensions + count * 4,
                       names + 4 + stride * count, sizes + count * 4)
    if hashes and not metadata_end <= hashes <= node.size:
        raise FormatError("DAT hash table offset outside metadata/payload boundary")
    result = []
    for i in range(count):
        slot = name_table[i * stride:(i + 1) * stride]
        end = slot.find(b"\0")
        if end <= 0:
            raise FormatError(f"DAT entry {i}: missing or unterminated name")
        try:
            name = slot[:end].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise FormatError(f"DAT entry {i}: invalid UTF-8 filename") from exc
        offset = struct.unpack_from("<I", offset_table, i * 4)[0]
        size = struct.unpack_from("<I", size_table, i * 4)[0]
        if offset + size > node.size or (size and offset < metadata_end):
            raise FormatError(f"DAT entry {i}: invalid payload extent")
        entry = Node(node.source, name, node.offset + offset, size, "unknown")
        result.append(Node(entry.source, name, entry.offset, size, identify(entry),
                           origin=node.origin))
    return result


def read_pkz(node: Node) -> list[Node]:
    """Index version 0x10000 PKZ; members remain compressed byte extents."""
    magic, version, size, count, table_offset, names_size = struct.unpack(
        "<4sIQIIQ", node.read(0, 32))
    if magic != b"pkzl" or version != 0x10000:
        raise FormatError("Unsupported PKZ signature/version")
    if size != node.size:
        raise FormatError("PKZ declared size does not match its extent")
    names_offset = table_offset + count * 32
    metadata_end = names_offset + names_size
    if table_offset < 32 or metadata_end > node.size:
        raise FormatError("PKZ tables exceed their parent extent")
    if count > 100_000 or count * 32 + names_size > 64 * 1024 * 1024:
        raise FormatError("PKZ table exceeds inspector resource limits")
    table = node.read(table_offset, count * 32)
    names = node.read(names_offset, names_size)
    if not names.startswith(b"ZStandard\0"):
        raise FormatError("Unsupported PKZ compression method")
    result = []
    for i in range(count):
        name_offset, expanded, offset, packed = struct.unpack_from("<QQQQ", table, i * 32)
        if name_offset < 16 or name_offset >= len(names):
            raise FormatError(f"PKZ entry {i}: invalid filename offset")
        end = names.find(b"\0", name_offset)
        if end < 0:
            raise FormatError(f"PKZ entry {i}: unterminated filename")
        try:
            name = names[name_offset:end].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise FormatError(f"PKZ entry {i}: invalid UTF-8 filename") from exc
        if not name or offset < metadata_end or offset + packed > size:
            raise FormatError(f"PKZ entry {i}: invalid name or payload extent")
        result.append(Node(node.source, name, node.offset + offset, packed,
                           "ZSTD member", expanded))
    return result


def read_table(node: Node) -> list[Node]:
    magic, count, strings_size, _ = struct.unpack("<4sIII", node.read(0, 16))
    if magic not in (b"HFS0", b"PFS0") or magic.decode() != node.kind:
        raise FormatError(f"Invalid {node.kind} table signature")
    stride = 64 if magic == b"HFS0" else 24
    header_size = 16 + count * stride + strings_size
    if header_size > node.size:
        raise FormatError("Container tables exceed their parent extent")
    # Reader resource limits, not claimed format limits. Fail rather than truncate.
    if count > 100_000 or header_size > 64 * 1024 * 1024:
        raise FormatError("Table exceeds inspector limit (100,000 entries / 64 MiB)")
    table = node.read(16, count * stride)
    strings = node.read(16 + count * stride, strings_size)
    result = []
    for index in range(count):
        relative, size, name_offset = struct.unpack_from("<QQI", table, index * stride)
        if name_offset >= strings_size:
            raise FormatError(f"Entry {index}: name offset outside string table")
        end = strings.find(b"\0", name_offset)
        if end < 0:
            raise FormatError(f"Entry {index}: unterminated filename")
        try:
            name = strings[name_offset:end].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise FormatError(f"Entry {index}: invalid UTF-8 filename") from exc
        if not name:
            raise FormatError(f"Entry {index}: empty filename")
        offset = header_size + relative
        if offset + size > node.size:
            raise FormatError(f"Entry {index}: payload outside parent extent")
        if magic == b"HFS0":
            hashed_size = struct.unpack_from("<I", table, index * stride + 20)[0]
            if hashed_size > size:
                raise FormatError(f"Entry {index}: hashed region exceeds payload")
        entry = Node(node.source, name, node.offset + offset, size, "unknown")
        result.append(Node(entry.source, name, entry.offset, size, identify(entry)))
    return result


def inventory(node: Node, depth: int = 0) -> dict:
    if depth > 32:
        raise FormatError("Nesting exceeds inspector limit (32 levels)")
    data = {"name": node.name, "kind": node.kind,
            "offset": node.offset, "size": node.size}
    if node.expanded_size is not None:
        data["expanded_size"] = node.expanded_size
    if node.browsable:
        data["children"] = [inventory(child, depth + 1) for child in children(node)]
    return data


def hex_preview(node: Node, limit: int = 512) -> str:
    data = node.read(0, min(node.size, limit))
    lines = []
    for start in range(0, len(data), 16):
        row = data[start:start + 16]
        printable = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
        lines.append(f"{node.offset + start:012X}  {row.hex(' '):47}  {printable}")
    return "\n".join(lines)
