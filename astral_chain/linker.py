"""Bounded offline linking of the observed NSO modules for runtime experiments."""
from dataclasses import dataclass
import struct
from .containers import FormatError
from .nso import Nso


@dataclass
class Module:
    name: str
    base: int
    nso: Nso


class LinkedImage:
    def __init__(self, exefs, *, relocate=True):
        self.modules = []
        base = 0x7000000000
        for name in ('rtld', 'main', 'sdk'):
            nso = Nso(exefs / name)
            self.modules.append(Module(name, base, nso))
            base = (base + nso.size + 0xffff) & ~0xffff
        # Executable definitions precede SDK definitions; unresolved weak imports
        # are zero per ELF semantics. No host service stubs are synthesized.
        self.symbols = {}
        for module in self.modules:
            for index, (name, info, section, value, size) in enumerate(module.nso.syms):
                if not name or not section or info >> 4 not in (1, 2) or module.nso.symbol_visibility[index] in (1, 2):
                    continue
                address = value if section == 0xfff1 else module.base + value
                old = self.symbols.get(name)
                if old is None or (old[1] == 2 and info >> 4 == 1):
                    self.symbols[name] = (address, info >> 4)
        self.weak_unresolved = set()
        self.relocation_count = 0
        if not relocate:
            return
        for module in self.modules:
            for target, kind, symbol, addend in module.nso.relocs:
                if kind == 0:
                    continue
                if kind == 1027:  # R_AARCH64_RELATIVE
                    if symbol:
                        raise FormatError('Relative relocation has a symbol')
                    value = module.base + addend
                elif kind in (257, 1025, 1026):  # ABS64, GLOB_DAT, JUMP_SLOT
                    name, info, section, address, _ = module.nso.syms[symbol]
                    if section and (info >> 4 == 0 or module.nso.symbol_visibility[symbol] != 0):
                        value = (address if section == 0xfff1 else module.base + address) + addend
                    elif name in self.symbols:
                        value = self.symbols[name][0] + addend
                    elif info >> 4 == 2:
                        self.weak_unresolved.add(name)
                        value = addend
                    else:
                        raise FormatError(f'Unresolved strong symbol: {module.name}:{name}')
                else:
                    raise FormatError(f'Unsupported relocation kind {kind}')
                if not 0 <= value < 1 << 64:
                    raise FormatError('Relocation result outside uint64')
                struct.pack_into('<Q', module.nso.image, target, value)
                self.relocation_count += 1

    def locate(self, address, *, text_only=False):
        for module in self.modules:
            lo, length = module.nso.text if text_only else (0, module.nso.size)
            if module.base + lo <= address < module.base + lo + length:
                return module, address-module.base
        raise FormatError(f'Address outside linked {"text" if text_only else "image"}: {address:#x}')

    def report(self):
        return {'modules': [{'name': m.name, 'base': m.base, 'size': m.nso.size,
                              'sha256': m.nso.sha256, 'build_id': m.nso.build_id} for m in self.modules],
                'relocations_applied': self.relocation_count, 'unresolved_strong': [],
                'unresolved_weak_zero': sorted(self.weak_unresolved)}
