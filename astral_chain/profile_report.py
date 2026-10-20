"""Symbolize AC_PROFILE sample files (profile-NN.txt) with the build's PDB through Windows dbghelp.

  python -m astral_chain.profile_report output/<run> [--top 40]

Each profile file starts with "base 0x..." (trace.exe load address), then "address count module" lines, then
guest-thread wait/run summaries (passed through). Samples in trace.exe are attributed to the containing function
(generated guest page functions keep their names, e.g. the guest address they translate); other modules are
grouped by module.
"""
import argparse
import collections
import ctypes
from ctypes import wintypes
from pathlib import Path
import re


class SymbolInfo(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wintypes.ULONG), ('TypeIndex', wintypes.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wintypes.ULONG), ('Size', wintypes.ULONG), ('ModBase', ctypes.c_uint64),
                ('Flags', wintypes.ULONG), ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64),
                ('Register', wintypes.ULONG), ('Scope', wintypes.ULONG), ('Tag', wintypes.ULONG),
                ('NameLen', wintypes.ULONG), ('MaxNameLen', wintypes.ULONG), ('Name', ctypes.c_char * 512)]


class Symbolizer:
    def __init__(self, exe: Path):
        self.dbghelp = ctypes.WinDLL('dbghelp')
        self.process = ctypes.c_void_p(0x1234)  # any unique pseudo handle: no live process is attached
        self.dbghelp.SymSetOptions(0x2)  # SYMOPT_UNDNAME (deferred loading left SymFromAddr without symbols)
        if not self.dbghelp.SymInitializeW(self.process, ctypes.c_wchar_p(str(exe.parent)), False):
            raise OSError('SymInitialize failed')
        self.dbghelp.SymLoadModuleExW.restype = ctypes.c_uint64
        self.base = 0x140000000
        loaded = self.dbghelp.SymLoadModuleExW(self.process, None, ctypes.c_wchar_p(str(exe)), None,
                                               ctypes.c_uint64(self.base), 0, None, 0)
        if not loaded:
            raise OSError(f'cannot load symbols for {exe}')
        self.cache = {}
        self.files = {}

    def guest_pc(self, offset: int):
        """Guest address of the translated case containing this host address (line info -> nearest case label)."""
        class Line(ctypes.Structure):
            _fields_ = [('SizeOfStruct', wintypes.DWORD), ('Key', ctypes.c_void_p), ('LineNumber', wintypes.DWORD),
                        ('FileName', ctypes.c_char_p), ('Address', ctypes.c_uint64)]
        line = Line()
        line.SizeOfStruct = ctypes.sizeof(Line)
        displacement = wintypes.DWORD()
        if not self.dbghelp.SymGetLineFromAddr64(self.process, ctypes.c_uint64(self.base + offset), ctypes.byref(displacement),
                                                 ctypes.byref(line)):
            return None
        path = line.FileName.decode(errors='replace')
        if path not in self.files:
            try:
                self.files[path] = Path(path).read_text(encoding='utf-8', errors='replace').splitlines()
            except OSError:
                self.files[path] = []
        lines = self.files[path]
        for n in range(min(line.LineNumber, len(lines)) - 1, max(-1, line.LineNumber - 400), -1):
            m = re.match(r'case (\d+)ull:', lines[n])
            if m:
                return int(m.group(1))
        return None

    def name(self, offset: int) -> str:
        if offset in self.cache:
            return self.cache[offset]
        info = SymbolInfo()
        info.SizeOfStruct = 88  # sizeof(SYMBOL_INFO) in C (Name[1] plus padding)
        info.MaxNameLen = 511
        displacement = ctypes.c_uint64()
        ok = self.dbghelp.SymFromAddr(self.process, ctypes.c_uint64(self.base + offset), ctypes.byref(displacement),
                                      ctypes.byref(info))
        result = info.Name.decode(errors='replace') if ok else f'trace.exe+{offset:#x}'
        self.cache[offset] = result
        return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('run', type=Path)
    parser.add_argument('--top', type=int, default=40)
    parser.add_argument('--files', default='profile-*.txt')
    parser.add_argument('--guest', help='function name: list its hot guest instruction addresses')
    args = parser.parse_args()
    exe = args.run/'round-00'/'trace.exe'
    sym = Symbolizer(exe)
    functions, modules = collections.Counter(), collections.Counter()
    callers = collections.defaultdict(collections.Counter)
    module_of = {}
    guest_pcs = collections.Counter()
    total = 0
    for path in sorted(args.run.glob(args.files)):
        base = None
        for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
            if line.startswith('base '):
                base = int(line.split()[1], 16)
                continue
            m = re.match(r'call (0x[0-9a-f]+) (0x[0-9a-f]+) (\d+)$', line)
            if m and base is not None:
                rip, caller, count = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3))
                in_exe = lambda a: 0 <= a - base < 0x40000000
                callee = sym.name(rip - base) if in_exe(rip) else f'[{module_of.get(rip, "other")}]'
                callers[callee][sym.name(caller - base) if in_exe(caller) else 'other module'] += count
                continue
            m = re.match(r'(0x[0-9a-f]+) (\d+) (\S+)$', line)
            if not m:
                continue
            address, count, module = int(m.group(1), 16), int(m.group(2)), m.group(3)
            module_of[address] = module  # (sample lines precede the call lines in each file)
            total += count
            modules[module] += count
            if module == 'trace.exe' and base is not None:
                functions[sym.name(address - base)] += count
                if args.guest and sym.name(address - base) == args.guest:
                    pc = sym.guest_pc(address - base)
                    guest_pcs[pc] += count
            else:
                functions[f'[{module}]'] += count
    print(f'{total} samples')
    for module, count in modules.most_common(8):
        print(f'{100 * count / total:6.1f}%  {module}')
    print()
    for name, count in functions.most_common(args.top):
        print(f'{100 * count / total:6.1f}%  {name}')
    if args.guest:
        print(f'\nguest instructions in {args.guest}:')
        for pc, count in guest_pcs.most_common(20):
            print(f'{100 * count / total:6.1f}%  {pc:#x}' if pc is not None else f'{100 * count / total:6.1f}%  ?')
    hot = [name for name, _ in functions.most_common(8) if name in callers]
    for name in hot:
        print(f'\ncallers of {name}:')
        for caller, count in callers[name].most_common(8):
            print(f'{100 * count / total:6.1f}%    {caller}')


if __name__ == '__main__':
    main()
