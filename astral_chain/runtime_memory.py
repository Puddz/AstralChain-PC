"""Guest memory-map metadata used by the bounded Windows runtime experiment."""
from dataclasses import dataclass
from .containers import FormatError

ADDRESS_LIMIT = 1 << 39
STACK_BASE, STACK_SIZE = 0x7100000000, 0x100000
TLS_BASE, TLS_SIZE = 0x7101000000, 0x10000


@dataclass(frozen=True)
class Region:
    base: int
    size: int
    kind: int
    permission: int


class MemoryMap:
    def __init__(self, regions):
        self.regions = sorted(regions, key=lambda r:r.base)
        end = 0
        for region in self.regions:
            if region.base < end or region.base % 4096 or region.size <= 0 or region.size % 4096 or region.base+region.size > ADDRESS_LIMIT:
                raise FormatError('Invalid/overlapping guest memory region')
            end = region.base+region.size

    @classmethod
    def from_image(cls, image, stack_size=STACK_SIZE):
        regions = []
        for module in image.modules:
            for (offset, size), kind, permission in ((module.nso.text, 3, 5), (module.nso.rodata, 3, 1),
                    ((module.nso.data[0], module.nso.data[1]+module.nso.bss_size), 4, 3)):
                if size:
                    regions.append(Region(module.base+offset, (size+4095)&~4095, kind, permission))
        regions.extend((Region(STACK_BASE, stack_size, 0xb, 3), Region(TLS_BASE, TLS_SIZE, 0xc, 3)))
        return cls(regions)

    def query(self, address):
        if not 0 <= address < 1 << 64:
            raise FormatError('Address outside uint64')
        if address >= ADDRESS_LIMIT:
            return Region(ADDRESS_LIMIT, (1<<64)-ADDRESS_LIMIT, 0x10, 0)
        low = 0
        for region in self.regions:
            if address < region.base:
                return Region(low, region.base-low, 0, 0)
            if address < region.base+region.size:
                return region
            low = region.base+region.size
        return Region(low, ADDRESS_LIMIT-low, 0, 0)

    def c_table(self):
        rows = ',\n'.join(f'{{{r.base}ull,{r.size}ull,{r.kind},{r.permission}}}' for r in self.regions)
        return 'typedef struct { uint64_t base,size; unsigned kind,permission; } Region;\nstatic const Region regions[]={\n'+rows+'\n};\n'
