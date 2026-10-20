"""Recompile bounded, register-only game routines and compare native x64 to ARM.

This is a translation validation tool, not a game boot/runtime. LASRecomp's
generic lifter is used directly; its Zelda patches and generator are not loaded.
"""
from pathlib import Path
import argparse
import ctypes
import hashlib
import json
import random
import struct
import subprocess
import sys

from .containers import FormatError
from .nso import Nso

LAS_COMMIT = "a0b48fd4138b180726dc732fe6bff78ca796be81"
MAIN_SHA = "74c14751a2e48d4a5da06b18164fb7b2104863dae604774bb190b0105ac457d4"
# Update v1.0.1 (v65536, build ID 4B159F0F7A360669): only main differs; rtld and sdk are byte-identical to the base.
MAIN_SHA_101 = "dc47cae835866379c597e1c507bf52ee0f865ec4412930dfdf1bbc20b6bb9c3b"
ACCEPTED_MAIN_SHAS = (MAIN_SHA, MAIN_SHA_101)
MODULE_SHAS = {'main': MAIN_SHA,
               'rtld': '52735462296339700ae8c54b3d84f8fd0d96113a313a15a4e0a6973ec63dffb4',
               'sdk': 'd58f76a49d32c1a8192befc8cd6341e45d802eade3a474f7117a842232446d7e'}
# Build-relative addresses. Semantic labels describe observed instructions only.
ROUTINES = [(0x3ba320, 48, "bit-membership"), (0x3bf8c0, 16, "byte-equals-46"),
            (0x551180, 16, "masked-field-equals-1"),
            (0x553820, 16, "field-bits-all-set"),
            (0x776350, 16, "conditional-offset"), (0x7882c0, 16, "alignment-result")]


def verify_reference(reference):
    # The vendored copy (third_party/LASRecomp: the files this project uses, at the pinned commit) records its
    # commit in COMMIT; a full git checkout is checked against the pin and for local changes.
    pinned = Path(reference)/'COMMIT'
    if pinned.is_file():
        if pinned.read_text(encoding='ascii').strip() != LAS_COMMIT:
            raise FormatError("LASRecomp revision differs from audited pin")
        return
    revision = subprocess.check_output(["git", "-C", str(reference), "rev-parse", "HEAD"], text=True).strip()
    if revision != LAS_COMMIT:
        raise FormatError("LASRecomp revision differs from audited pin")
    if subprocess.run(['git','-C',str(reference),'diff','--quiet','HEAD','--','tools','runtime'],timeout=10).returncode:
        raise FormatError('LASRecomp tracked tools/runtime differ from pinned source')


def generate(nso, reference):
    import capstone
    verify_reference(reference)
    sys.path.insert(0, str(reference / "tools"))
    from lifter import Lifter
    lifter = Lifter(0, simd="c")
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    source = ['#include "cpu.h"', '/* Generated locally from the identified user game build. */']
    manifest = []
    allowed = {"cmp", "b.hi", "b.eq", "mov", "lsl", "movk", "tst", "ret", "and", "cset", "bics", "add", "csel", "csinc"}
    for index, (start, size, label) in enumerate(ROUTINES):
        code = bytes(nso.image[start:start+size])
        decoded = list(md.disasm(code, start))
        if len(decoded)*4 != size or any(i.mnemonic not in allowed or "[" in i.op_str for i in decoded):
            raise FormatError("Probe contains instructions outside the reviewed register-only subset")
        source.append(f'static void probe_{index}(Cpu *c) {{')
        for instruction in decoded:
            pc = instruction.address
            lifted = lifter.lift(struct.unpack("<I", instruction.bytes)[0], pc)
            source.append(f'L_{pc:x}:;')
            if lifted.kind == "op":
                source.append(lifted.code)
            elif lifted.kind == "ret":
                source.append("return;")
            elif lifted.kind == "bcond":
                if not start <= lifted.target < start+size or lifted.target % 4:
                    raise FormatError("Probe branch escapes function")
                source.append(f'if ({lifted.cond}) goto L_{lifted.target:x};')
            else:
                raise FormatError(f"Unsupported probe control flow: {lifted.kind}")
        source.append('}')
        manifest.append({"label": label, "offset": start, "size": size,
                         "sha256": hashlib.sha256(code).hexdigest(),
                         "disassembly": [f'{i.address:x}: {i.mnemonic} {i.op_str}' for i in decoded]})
    source.append('__declspec(dllexport) void run_probe(unsigned index, uint64_t *state) { Cpu cpu = {0}; Cpu *c = &cpu; memcpy(c->x, state, 31*8); c->nzcv=state[31]; switch(index) {')
    for i in range(len(ROUTINES)):
        source.append(f'case {i}: probe_{i}(c); break;')
    source.append('} memcpy(state,c->x,31*8); state[31]=c->nzcv; }')
    return "\n".join(source), manifest


def validate(nso, dll):
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
    from unicorn import arm64_const as arm
    native = ctypes.CDLL(str(dll))
    native.run_probe.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]
    native.run_probe.restype = None
    machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    stop = 0x2000000
    machine.mem_map(0, (nso.text[1]+4095)&~4095)
    machine.mem_write(nso.text[0], bytes(nso.image[nso.text[0]:sum(nso.text)]))
    machine.mem_map(stop, 4096)
    registers = [getattr(arm, f'UC_ARM64_REG_X{i}') for i in range(31)]
    rng = random.Random(0x41535452)
    counts = []
    for index, (start, _, label) in enumerate(ROUTINES):
        inputs = [(x, y) for x in range(256) for y in (0, 1)]
        inputs += [(rng.getrandbits(64), rng.getrandbits(64)) for _ in range(512)]
        inputs += [(x, y) for x in (0, 0xffffffff, 0xffffffffffffffff, 0x8000000000000000, 0xf0000, 0x10000) for y in (0, 1, 0x100000000)]
        for x, y in inputs:
            state = [rng.getrandbits(64) for _ in range(31)] + [rng.randrange(16)<<28]
            state[0], state[1], state[30] = x, y, stop
            for register, value in zip(registers, state):
                machine.reg_write(register, value)
            machine.reg_write(arm.UC_ARM64_REG_NZCV, state[31])
            machine.emu_start(start, stop, timeout=100000, count=100)
            if machine.reg_read(arm.UC_ARM64_REG_PC) != stop:
                raise AssertionError("ARM reference did not return within bounds")
            expected = [machine.reg_read(r) for r in registers] + [machine.reg_read(arm.UC_ARM64_REG_NZCV)]
            actual = (ctypes.c_uint64 * 32)(*state)
            native.run_probe(index, actual)
            if list(actual) != expected:
                raise AssertionError(f'Native/ARM mismatch {label}, x0={x:#x}, x1={y:#x}')
        counts.append({"label": label, "cases": len(inputs), "all_gprs_and_nzcv_match": True})
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("main", type=Path)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    nso = Nso(args.main)
    if nso.sha256 != MAIN_SHA:
        raise FormatError("Unknown main build; probe addresses require reviewed identity")
    reference = args.reference.resolve()
    source, manifest = generate(nso, reference)
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    (output / "probe.c").write_text(source, encoding="utf-8")
    command = [str(args.compiler.resolve()), "cc", "-target", "x86_64-windows-gnu", "-O2", "-shared",
               "-I", str(reference / "runtime"), str(output / "probe.c"), "-o", str(output / "probe.dll")]
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=180)
    (output / "compiler.log").write_text(compiled.stdout+compiled.stderr, encoding="utf-8")
    compiled.check_returncode()
    counts = validate(nso, output / "probe.dll")
    report = {"main_sha256": nso.sha256, "build_id": nso.build_id, "las_commit": LAS_COMMIT,
              "host_target": "x86_64-windows-gnu", "compiler_command": command,
              "routines": manifest, "validation": counts,
              "scope": "Register-only native routine validation; ARM interpreter used only as test oracle; no game startup."}
    (output / "report.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    print(f'Native x64 validation passed: {sum(c["cases"] for c in counts)} cases across {len(counts)} game routines.')


if __name__ == "__main__":
    main()
