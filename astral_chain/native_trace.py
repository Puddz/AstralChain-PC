"""Bounded native startup-function experiment; stops at unimplemented boundaries.

This invokes a selected entry in a zero-initialized harness, not Horizon's complete
process startup. Generated code/images stay in the explicitly chosen output dir.
"""
from collections import deque
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
from time import perf_counter

from .containers import FormatError
from .linker import LinkedImage
from .native_probe import ACCEPTED_MAIN_SHAS, LAS_COMMIT, MODULE_SHAS, verify_reference
from .runtime_memory import MemoryMap, STACK_BASE, TLS_BASE, TLS_SIZE
from .process_meta import read_process_meta
from .control_meta import LANGUAGES, read_control_meta
from .account_profile import load_profile, account_runtime_constants
from .save_config import save_configuration, save_runtime_constants
from .romfs_image import inspect_romfs, runtime_constants
from .static_targets import plt_got, import_hints, switch_targets, initialization_targets, adjacent_code_targets, relocated_code_targets, CODE_POINTER_RADIUS
from .block_codegen import emit_blocks
from .lift_extensions import lift
from .native_build import split_dispatch, build as cached_build, run_compiler
from .nvn_build import build_nvn_objects, GLSLANG


def whole_text_ranges(image, limit):
    """Compact, bounded coverage roots; text contents need not all be code."""
    ranges=[]
    for module in image.modules:
        offset,size=module.nso.text
        if offset<0 or size<=0 or (module.base+offset)%4 or size%4:
            raise FormatError('Whole-text coverage requires aligned nonempty segments')
        ranges.append(range(module.base+offset,module.base+offset+size,4))
    if sum(len(r) for r in ranges)>limit:
        raise FormatError('Whole-text coverage exceeds instruction budget')
    return ranges


def translate(image, reference, seeds, limit, nodes=None, *, all_text=False):
    dense_ranges=whole_text_ranges(image,limit) if all_text else ()
    verify_reference(reference)
    sys.path.insert(0, str(reference/'tools'))
    from lifter import Lifter, Unsupported
    queue, seen, pages, unsupported = deque(seeds), set(), {}, []
    for addresses in dense_ranges:
        queue.extend(addresses)
    hints=import_hints(image)
    followed_imports=set()
    followed_switches=set()
    while queue:
        pc = queue.popleft()
        if pc in seen:
            continue
        try:
            module, offset = image.locate(pc, text_only=True)
        except FormatError:
            continue
        if pc % 4 or offset+4 > sum(module.nso.text):
            raise FormatError('Unaligned/incomplete instruction')
        if len(seen)>=limit:
            queue.appendleft(pc)
            break
        seen.add(pc)
        word = module.nso.u32(offset)
        try:
            op = lift(Lifter(module.base, simd='c'),word,pc)
        except Unsupported as exc:
            if nodes is not None: nodes[pc]=(word,None)
            unsupported.append({'pc': pc, 'reason': str(exc)})
            pages.setdefault(pc>>12,[]).append(f'case {pc}ull: boundary("unsupported", c, {pc}ull);')
            continue
        if nodes is not None: nodes[pc]=(word,op)
        after = pc+4
        body = f'case {pc}ull: {{ '
        if op.kind == 'op':
            body += op.code + f' c->pc={after}ull; break;'
            queue.append(after)
        elif op.kind in ('b', 'bl'):
            if op.kind == 'bl':
                body += f'c->x[30]={after}ull; trace_call({pc}ull, {op.target}ull); '
                queue.append(after)
            body += f'c->pc={op.target}ull; break;'
            queue.append(op.target)
        elif op.kind == 'bcond':
            body += f'c->pc=({op.cond}) ? {op.target}ull : {after}ull; break;'
            queue.extend((op.target, after))
        elif op.kind in ('br', 'blr'):
            if op.kind=='br' and offset>=module.nso.text[0]+12:
                got=plt_got(module.nso.unpack('<4I',offset-12),pc)
                if got in hints:
                    queue.extend(hints[got]);followed_imports.add(got)
            if op.kind=='br' and offset>=module.nso.text[0]+24:
                count=min(32,(offset-module.nso.text[0])//4+1)
                targets=switch_targets(module,module.nso.unpack('<'+str(count)+'I',offset-(count-1)*4),pc)
                if targets:
                    queue.extend(targets);followed_switches.add(pc)
            body += f'uint64_t target={op.target}; '
            if op.kind == 'blr':
                body += f'c->x[30]={after}ull; trace_call({pc}ull,target); '
                queue.append(after)
            body += 'c->pc=target; break;'
        elif op.kind == 'ret':
            body += f'c->pc=c->x[{(word>>5)&31}]; break;'
        elif op.kind == 'trap':
            body += op.code
        else:
            unsupported.append({'pc': pc, 'reason': op.kind})
            body += f'boundary("unsupported", c, {pc}ull);'
        pages.setdefault(pc>>12,[]).append(body+' }')
    return pages, {'translated_instructions': len(seen), 'code_pages':len(pages), 'import_hints_followed':len(followed_imports), 'switch_tables_followed':len(followed_switches), 'pending_static_targets': len(queue), 'unsupported': unsupported}


# SDK exports with host implementations in runtime nvn_hle.h (hle_symbol_table).
HLE_SYMBOLS=('_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE',
             '_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE',
             '_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE')
CHAIN_UNSAFE=('rt_svc','rt_brk','rt_udf','boundary','thread_','hle_')


def chain_cases(cases):
    """Let straight-line instructions fall through to their successor case.

    Only a case that ends by setting pc to the next instruction, assigns pc
    nowhere else and calls nothing that can switch context, trap or stop is
    chained. Scheduler and budget checks then run per block, not per
    instruction; guest semantics are unchanged.
    """
    ordered=sorted(cases,key=lambda text:int(text[5:text.index('ull')]))
    addresses={int(text[5:text.index('ull')]) for text in ordered}
    result=[]
    for text in ordered:
        address=int(text[5:text.index('ull')])
        tail=f'c->pc={address+4}ull; break; }}'
        if (text.endswith(tail) and address+4 in addresses and (address+4)>>12==address>>12
                and text.count('c->pc=')==1 and not any(word in text for word in CHAIN_UNSAFE)):
            text=text[:-len(tail)]+'}'
        result.append(text)
    return result


# --restrict: page functions promise the compiler that guest memory accesses never alias the Cpu context (guest
# memory is never host Cpu storage), so guest registers need not be reloaded after every guest store.
PAGE_RESTRICT = ''
# --lean (parallel mode only): no per-block step counter / scheduler checkpoint and no call trace in page functions.
# Guest threads then run on host threads without cooperative preemption (the runtime forces AC_PARALLEL).
PAGE_LEAN = False
# Lean builds: a BL to a translated page calls that page's function directly instead of returning to execute()
# (call and return each cost a dispatcher round trip). The callee returns to the caller's frame once guest
# execution leaves its page; the caller then continues at the return address if it is back in its own page,
# else returns too. Targets the runtime intercepts (HLE overrides, nvnBootstrapLoader) keep the dispatcher.
DIRECT_PAGES = set()
DIRECT_EXCLUDE = set()
_BL_TAIL = re.compile(r'c->x\[30\]=(\d+)ull; c->pc=(\d+)ull; break; \}$')


def _direct_call(text, called):
    m = _BL_TAIL.search(text)
    if not m:
        return text
    target = int(m.group(2))
    if target >> 12 not in DIRECT_PAGES or target in DIRECT_EXCLUDE:
        return text
    called.add(target >> 12)
    return text[:m.start()] + f'c->x[30]={m.group(1)}ull; c->pc={target}ull; page_{target >> 12:x}(c); break; }}'


# Lean builds with --jumps: a branch to a case of the same page jumps there directly (goto) instead of leaving
# the case for the page loop's switch (an indirect jump per taken branch). c->pc is still set, so exits work.
PAGE_JUMPS = False
_JUMP_TAIL = re.compile(r'c->pc=(\d+)ull; break; \}$')
_COND_TAIL = re.compile(r'c->pc=\((.*)\) \? (\d+)ull : (\d+)ull; break; \}$')


def _local_jumps(cases, page):
    labels = {int(t[5:t.index('ull')]) for t in cases}
    used, out = set(), []
    def go(target):
        if target in labels:
            used.add(target)
            return f'c->pc={target}ull; goto L_{target:x};'
        return f'c->pc={target}ull; break;'
    for text in cases:
        m = _COND_TAIL.search(text)
        if m and (int(m.group(2)) in labels or int(m.group(3)) in labels):
            text = text[:m.start()] + f'if ({m.group(1)}) {{ {go(int(m.group(2)))} }} {go(int(m.group(3)))} }}'
        else:
            m = _JUMP_TAIL.search(text)
            if m and int(m.group(1)) in labels:
                text = text[:m.start()] + go(int(m.group(1))) + ' }'
        out.append(text)
    return [(f'L_{int(t[5:t.index("ull")]):x}: ' if int(t[5:t.index('ull')]) in used else '') + t for t in out]


def emit_page(page,cases,limit,stop_at=None,external=False,chain=False):
    if PAGE_LEAN and stop_at is None:
        cases=[re.sub(r'trace_call\([^;]*\); ','',text) for text in cases]
        called=set()
        if DIRECT_PAGES and external:  # (cached builds: page functions are external symbols)
            cases=[_direct_call(text,called) for text in cases]
        if PAGE_JUMPS:
            cases=sorted(cases,key=lambda text:int(text[5:text.index('ull')]))
            if chain: cases=chain_cases(cases)
            cases=_local_jumps(cases,page)
            chain=False  # (already chained)
        protos=''.join(f'void page_{p:x}(Cpu*);\n' for p in sorted(called - {page}))
        parts=[f'{protos}{"" if external else "static "}__attribute__((noinline)) void page_{page:x}(Cpu *{PAGE_RESTRICT}c) {{\nwhile((c->pc>>12)=={page}ull) {{\nswitch(c->pc) {{']
        parts.extend(cases if PAGE_JUMPS else chain_cases(cases) if chain else sorted(cases))  # (--jumps: ordered and chained above)
        parts.append('default: boundary("untranslated",c,c->pc);\n} } }')
        return '\n'.join(parts)
    return emit_page_counted(page,cases,limit,stop_at,external,chain)


def emit_page_counted(page,cases,limit,stop_at=None,external=False,chain=False):
    # Only the page holding the stop address needs the check: other pages leave
    # through execute() before reaching it, so their cached objects stay valid.
    stop=f'if(c->pc=={stop_at}ull) boundary("pc-test-boundary",c,c->pc);' if stop_at is not None and stop_at>>12==page else ''
    linkage='' if external else 'static '
    budget='native_instruction_limit' if external else f'{limit}ull'
    parts=[f'{linkage}__attribute__((noinline)) void page_{page:x}(Cpu *{PAGE_RESTRICT}c) {{\nwhile((c->pc>>12)=={page}ull) {{\n{stop}\nif(thread_reschedule || (executed_steps & 4095)==0) {{if(thread_checkpoint(c,executed_steps)) return;}}\nif(executed_steps++>={budget}) boundary("instruction-budget",c,c->pc);\nswitch(c->pc) {{']
    parts.extend(chain_cases(cases) if chain and not stop else sorted(cases))
    parts.append('default: boundary("untranslated",c,c->pc);\n} } }')
    return '\n'.join(parts)


def emit_execute(pages,stop_at=None):
    # Runtimes without host interception (minimal test runtimes) omit the hook.
    guard='#ifndef HLE_INTERCEPT\n#define HLE_INTERCEPT(c)\n#endif\n'
    stop=f'if(c->pc=={stop_at}ull) boundary("pc-test-boundary",c,c->pc);' if stop_at is not None else ''
    parts=[guard+'static void execute(Cpu *c) { for(;;) { if(thread_reschedule) thread_checkpoint(c,executed_steps); '+stop+' if(c->pc==0x7fffffffffffull) { puts("RETURN");return; } HLE_INTERCEPT(c) switch(c->pc>>12) {']
    for page in sorted(pages):
        parts.append(f'case {page}ull: page_{page:x}(c); break;')
    parts.append('default: boundary("untranslated",c,c->pc); } } }')
    return '\n'.join(parts)


def emit_dispatch(pages, limit, stop_at=None, chain=False):
    """Bound C compiler complexity by putting each guest code page in a function."""
    return '\n'.join(['static __thread uint64_t executed_steps;',
        *(emit_page(page,cases,limit,stop_at,chain=chain) for page,cases in sorted(pages.items())),
        emit_execute(pages,stop_at)])


RUNTIME = (Path(__file__).parent / "runtime/host_runtime.c.in").read_text(encoding="utf-8")


def apply_pchtxt(image, path):
    """Apply the enabled sections of a pchtxt (IPS-style text) patch to main's loaded image.

    `@nsobid-<id>` must match main's build ID. Offsets are NSO file offsets (header included) plus
    `@flag offset_shift`; each value is the replacement bytes in file order. Patches must stay inside main's
    text segment. Translation and the guest memory image both use the patched bytes.
    """
    main = next(m for m in image.modules if m.name == 'main')
    build_id = main.nso.build_id if isinstance(main.nso.build_id, str) else main.nso.build_id.hex()
    shift, enabled, words, seen_id = 0, False, 0, None
    text_lo, text_size = main.nso.text
    # The text mapping is page-rounded and executable: code caves may use the zero padding after the text bytes
    # (the Dynamic-FPS mod puts its new code at main+0xA0E7D0, the exact end of v1.0.1's text). Never past rodata.
    mapped_end = min((text_lo + text_size + 0xfff) & ~0xfff, main.nso.rodata[0])
    patched_end = text_lo + text_size
    for number, raw in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        line = raw.split('//')[0].strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('@nsobid-'):
            seen_id = line[8:].lower()
            if not build_id.lower().startswith(seen_id[:16]):
                raise FormatError(f'{path}: patch build ID {seen_id} does not match main {build_id}')
        elif line.startswith('@flag offset_shift'):
            shift = int(line.split()[-1], 0)
        elif line == '@enabled':
            enabled = True
        elif line == '@disabled':
            enabled = False
        elif line == '@stop':
            break
        elif enabled:
            m = re.fullmatch(r'([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]+)', line)
            if not m or len(m.group(2)) % 2:
                raise FormatError(f'{path}:{number}: unsupported patch line')
            if seen_id is None:
                raise FormatError(f'{path}: no @nsobid before the first patch')
            offset, data = int(m.group(1), 16) + shift - 0x100, bytes.fromhex(m.group(2))
            if not (text_lo <= offset and offset + len(data) <= mapped_end):
                raise FormatError(f'{path}:{number}: patch outside main text')
            main.nso.image[offset:offset + len(data)] = data
            patched_end = max(patched_end, offset + len(data))
            words += 1
    if patched_end > text_lo + text_size:  # translate the cave too (whole-text seeding reads nso.text)
        main.nso.text = (text_lo, (patched_end - text_lo + 3) & ~3)
    return words


def cache_image(cache, data):
    cache.mkdir(parents=True,exist_ok=True)
    digest=hashlib.sha256(data).hexdigest()
    path=cache/(digest+'.image')
    if path.exists():
        if path.stat().st_size!=len(data):
            raise FormatError('Cached module image size mismatch')
        with path.open('rb') as stream:
            if hashlib.file_digest(stream,'sha256').hexdigest()!=digest:
                raise FormatError('Cached module image content mismatch')
    else:
        with path.open('xb') as stream:
            stream.write(data)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exefs', type=Path)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--entry', default='nninitStartup')
    coverage=parser.add_mutually_exclusive_group()
    coverage.add_argument('--seed-module', choices=('rtld', 'main', 'sdk'))
    coverage.add_argument('--seed-all-text',action='store_true',help='Cover every aligned instruction word in all executable text segments; keep unsupported words explicit')
    parser.add_argument('--seed-initializers',action='store_true',help='Precompile bounded module initialization-array targets')
    parser.add_argument('--seed-report', type=Path)
    parser.add_argument('--stop-first-svc', action='store_true', help='Independent validation boundary before any host syscall')
    parser.add_argument('--stop-at',type=lambda v:int(v,0),help='Stop before executing this guest address for validation')
    parser.add_argument('--seed', action='append', type=lambda v:int(v,0), default=[])
    parser.add_argument('--instructions', type=int, default=2048)
    parser.add_argument('--dispatch',choices=('pages','blocks'),default='pages',help='Instruction-switch baseline or direct-label block backend')
    parser.add_argument('--build',choices=('single','cached'),default='single',help='Single C unit or cached groups of code pages')
    parser.add_argument('--seed-code-pointers',action='store_true',help='Bounded inventory of relocated intra-module text-pointer hints')
    parser.add_argument('--seed-pointer-neighbors',action='store_true',help='Bounded adjacent relocated code-pointer hints')
    parser.add_argument('--follow', type=int, default=0, help='Bounded extra compile/run rounds for newly observed targets')
    parser.add_argument('--steps', type=int, default=100000000)
    parser.add_argument('--sample-steps',type=int,default=0,help='Optional register sampling interval at scheduler checkpoints (0 disables, otherwise 4096..1000000000)')
    parser.add_argument('--syscalls',type=int,default=100000,help='Diagnostic syscall budget (1..1000000)')
    parser.add_argument('--calls',type=int,default=1000000,help='Diagnostic call budget (1..100000000)')
    parser.add_argument('--run-timeout', type=int, default=120, help='Seconds allowed per native run (1..600)')
    parser.add_argument('--optimize',action='store_true',help='Build generated code and runtime with -O2 for x86-64-v3 (AVX2/FMA CPUs); separate object cache')
    parser.add_argument('--build-only',action='store_true',help='Compile and link trace.exe, then stop without running the game')
    parser.add_argument('--chain',action='store_true',help='Fall through straight-line instructions within a page (faster; budgets and checkpoints per block)')
    parser.add_argument('--unbounded',action='store_true',help='Interactive play: no call/syscall/instruction budgets, up to 24 h, output streamed to trace.log')
    parser.add_argument('--experimental-vulkan-package',type=Path,help='Opt in to reviewed first-draw Vulkan experiment; not general rendering')
    parser.add_argument('--nvn-hle',action='store_true',help='Opt in to NVN API interception at nvnBootstrapLoader; unimplemented functions stop by name')
    parser.add_argument('--nvn-backend',choices=('null','vulkan'),help='Link vendored LASRecomp NVN implementations with this backend (requires --nvn-hle)')
    parser.add_argument('--performance-mode', choices=('normal','boost'), default='boost', help='Guest compatibility mode; does not change host clocks')
    parser.add_argument('--control',type=Path,help='Extracted control.nacp for supported application languages')
    parser.add_argument('--romfs',type=Path,help='Raw decrypted RomFS image, opened read-only by the host')
    parser.add_argument('--restrict',action='store_true',help="Declare page functions' Cpu pointer restrict (performance)")
    parser.add_argument('--lean',action='store_true',help='No step counter, checkpoints or call trace in translated code (forces AC_PARALLEL)')
    parser.add_argument('--direct-calls',action='store_true',help='With --lean: BL to a translated page calls its function directly')
    parser.add_argument('--jumps',action='store_true',help='With --lean: branches within a page jump to their case directly')
    parser.add_argument('--patch',type=Path,action='append',default=[],help='pchtxt code patch for main (enabled sections; checked against its build ID)')
    parser.add_argument('--account-profile',type=Path,default=Path(__file__).resolve().parent.parent/'userdata/account.json',help='Persistent local PC profile JSON, created once if missing')
    parser.add_argument('--language',choices=LANGUAGES,default='en-GB',help='Preferred language; falls back to supported English or first supported locale')
    args = parser.parse_args()
    if args.restrict:
        # (run as __main__: native_build's split_dispatch imports this file again as astral_chain.native_trace)
        global PAGE_RESTRICT
        PAGE_RESTRICT = 'restrict '
        from . import native_trace as imported
        imported.PAGE_RESTRICT = PAGE_RESTRICT
    if args.lean:
        global PAGE_LEAN
        PAGE_LEAN = True
        from . import native_trace as imported
        imported.PAGE_LEAN = True
        imported.PAGE_JUMPS = args.jumps
    if args.optimize:
        # Shared list: native_build and nvn_build key their caches on it.
        from . import native_build
        native_build.FLAGS[:]=['-target','x86_64-windows-gnu','-O2','-mcpu=x86_64_v3','-mcx16']
    if args.nvn_backend and not args.nvn_hle:
        parser.error('--nvn-backend requires --nvn-hle')
    if args.build=='cached' and args.dispatch!='pages':
        parser.error('Cached builds currently require the pages dispatcher')
    if args.sample_steps != 0 and not 4096 <= args.sample_steps <= 1000000000:
        parser.error('sample interval must be 0 or 4096..1000000000')
    if args.unbounded:
        # Counters are 32-bit unsigned in the runtime; at UINT32_MAX the comparison never trips.
        args.calls,args.syscalls,args.steps=0xffffffff,0xffffffff,(1<<63)-1
        if not 1<=args.run_timeout<=86400:
            parser.error('--run-timeout must be in 1..86400')
    elif not 1 <= args.syscalls <= 1000000:
        parser.error('syscall budget must be 1..1000000')
    if not args.unbounded and not 1 <= args.calls <= 100000000:
        parser.error('call budget must be 1..100000000')
    if not 1 <= args.instructions <= 5000000:
        parser.error('instruction budget must be 1..5000000')
    if not 0 <= args.follow <= 30:
        parser.error('follow budget must be 0..30')
    if not args.unbounded and not 1 <= args.run_timeout <= 600:
        parser.error('--run-timeout must be in 1..600')
    if not args.unbounded and not 1 <= args.steps <= 10000000000:
        parser.error('step budget must be 1..10000000000')
    image = LinkedImage(args.exefs, relocate=args.entry != 'rtld:start')
    if args.stop_at is not None:
        image.locate(args.stop_at,text_only=True)
        if args.stop_at%4: parser.error('--stop-at must be instruction aligned')
    process = read_process_meta(args.exefs/'main.npdm')
    control = read_control_meta(args.control,args.language) if args.control else None
    romfs = inspect_romfs(args.romfs) if args.romfs else None
    if any(module.nso.sha256 not in (ACCEPTED_MAIN_SHAS if module.name == 'main' else (MODULE_SHAS[module.name],))
           for module in image.modules):
        raise FormatError('Unknown/mixed executable build')
    for patch in args.patch:
        print(f'Applied {apply_pchtxt(image, patch)} patch words from {patch}', flush=True)
    if args.seed_all_text:
        whole_text_ranges(image,args.instructions)
    entry = image.modules[0].base if args.entry == 'rtld:start' else image.symbols[args.entry][0]
    profile = load_profile(args.account_profile)
    save_config = save_configuration(process, control, profile, args.account_profile)
    reference = args.reference.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    memory = MemoryMap.from_image(image, process['stack_size'])
    targets = [m.base+r[0] for m in image.modules for r in m.nso.relocs if r[1]]
    (output/'relocation-targets.bin').write_bytes(struct.pack(f'<{len(targets)}Q',*targets))
    constants = f'#define STACK_BASE {STACK_BASE}ull\n#define STACK_SIZE {process["stack_size"]}ull\n#define MAIN_PRIORITY {process["priority"]}\n#define MAIN_CORE {process["default_core"]}\n#define STOP_FIRST_SVC {int(args.stop_first_svc)}\n#define SYSTEM_RESOURCE_SIZE {process["system_resource_size"]}ull\n'
    constants += account_runtime_constants(profile)
    constants += save_runtime_constants(save_config)
    constants += f'#define ACCOUNT_SWITCH_LOCK {control["user_account_switch_lock"] if control else -1}\n'
    constants += f'#define EXECUTION_SAMPLE_STEPS {args.sample_steps}ull\n'
    constants += f'#define SYSCALL_LIMIT {args.syscalls}u\n'
    constants += f'#define PROCESS_CORE_MASK {process["core_mask"]}ull\n#define PROCESS_PRIORITY_MASK {process["priority_mask"]}ull\n'
    constants += f'#define PERFORMANCE_MODE {int(args.performance_mode=="boost")}\n'
    constants += f'#define LEAN_PAGES {int(args.lean)}\n'
    constants += f'#define APPLICATION_LANGUAGE {control["language_code"] if control else 0}ull\n'
    system_language=int.from_bytes(args.language.encode('ascii').ljust(8,b'\0'),'little')
    constants += f'#define SYSTEM_LANGUAGE {system_language}ull\n'
    runtime_source = f'#define CALL_LIMIT {args.calls}\n'+constants+RUNTIME.replace('/*MEMORY_TABLE*/',memory.c_table())
    runtime_source = '#include <stddef.h>\n'+runtime_constants(romfs)+runtime_source
    setup = '''\nint main(int argc, char **argv) {
SetUnhandledExceptionFilter(fatal_fault_handler);
snapshot_path=argc>1 ? argv[1] : NULL;
if(BCryptGenRandom(NULL,(PUCHAR)entropy,sizeof(entropy),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0) return 34;
'''
    for module in image.modules:
        image_file=cache_image(output.parent/'native-images',module.nso.image)
        setup += f'reserve({module.base}ull,{module.nso.size},{json.dumps(image_file.as_posix())});\n'
    seeds = [entry]+args.seed
    if args.seed_initializers:
        for module in image.modules:
            seeds.extend(initialization_targets(module))
    if args.seed_code_pointers:
        for module in image.modules:
            seeds.extend(relocated_code_targets(module))
    if args.seed_report:
        if args.seed_report.stat().st_size > 4*1024*1024:
            raise FormatError('Seed report exceeds limit')
        previous = json.loads(args.seed_report.read_text(encoding='utf-8'))
        if previous['linker']['modules'] != image.report()['modules']:
            raise FormatError('Seed report module identity/layout mismatch')
        extra = previous['seeds']
        if len(extra)>50000 or any(type(p) is not int for p in extra):
            raise FormatError('Invalid seed report targets')
        seeds.extend(extra)
    seeds=list(dict.fromkeys(seeds))
    if len(seeds)>50000:
        raise FormatError('Combined seed inventory exceeds limit')
    if args.seed_module:
        module = next(m for m in image.modules if m.name == args.seed_module)
        if module.nso.text[1]//4 > args.instructions:
            raise FormatError('Whole-module seed exceeds instruction budget')
        seeds.extend(range(module.base+module.nso.text[0], module.base+sum(module.nso.text), 4))
    rounds = []
    def checkpoint(status, translation, exit_code=None):
        report = {'linker': image.report(), 'process': process, 'translation': translation, 'entry': args.entry,
                  'entry_address': entry, 'las_commit': LAS_COMMIT, 'exit_code': exit_code,
                  'stop_at': args.stop_at,
                  'dispatch':args.dispatch,
                  'call_budget':args.calls,
                  'syscall_budget':args.syscalls,
                  'sample_steps':args.sample_steps,
                  'step_budget':args.steps,
                  'relocated_code_hints':args.seed_code_pointers,
                  'whole_text_coverage':args.seed_all_text,
                  'pointer_neighbor_hints':args.seed_pointer_neighbors,
                  'pointer_neighbor_radius':CODE_POINTER_RADIUS if args.seed_pointer_neighbors else 0,
                  'build':args.build,
                  'account_profile':{'path':str(args.account_profile.resolve()),'uid':profile['uid'],'kind':'local-pc'},
                  'control':control,
                  'save_store':save_config,
                  'romfs':romfs,
                  'system_language':args.language,
                  'rounds': rounds, 'seeds': seeds, 'status': status, 'performance_mode': args.performance_mode, 'nvn_hle': args.nvn_hle, 'nvn_backend': args.nvn_backend, 'chain': args.chain, 'optimize': args.optimize,
                  'scope': 'NPDM-sized stack, zero-filled TLS via TPIDRRO_EL0, main thread handle 0x1000; rtld:start uses unrelocated modules. Named entries are isolated experiments without prior SDK initialization.'}
        (output/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    pointer_observed=set(seeds)
    for iteration in range(args.follow+1):
        if args.seed_pointer_neighbors:
            known=set(seeds)
            for module in image.modules:
                seeds.extend(target for target in adjacent_code_targets(module,pointer_observed) if target not in known)
        nodes={} if args.dispatch=='blocks' else None
        pages, translation = translate(image, reference, seeds, args.instructions,nodes,all_text=args.seed_all_text)
        if args.lean and args.direct_calls:
            from . import native_trace as imported
            imported.DIRECT_PAGES = set(pages)
            imported.DIRECT_EXCLUDE = {image.symbols[name][0] for name in (*HLE_SYMBOLS, 'nvnBootstrapLoader') if name in image.symbols}
        checkpoint('compiling',translation)
        dispatch=emit_blocks(nodes,args.steps,args.stop_at) if nodes is not None else emit_dispatch(pages,args.steps,args.stop_at,chain=args.chain)
        units=None
        if args.build=='cached':
            units,dispatch=split_dispatch(pages,args.steps,args.stop_at,chain=args.chain)
        source = runtime_source+'\n'+dispatch+'\n'+setup+f'''reserve(STACK_BASE,STACK_SIZE,NULL);
reserve({TLS_BASE}ull,{TLS_SIZE},NULL);
Cpu *c=&state; c->sp=STACK_BASE+STACK_SIZE-16; c->tpidrro={TLS_BASE}ull;
c->x[1]=0x1000;
c->pc={entry}ull; c->x[30]=0x7fffffffffffull;
if(!reserve_heap_range()) boundary("host-heap-reservation",c,c->pc);
guest_execute=execute;
execute(c); return 0; }}
'''
        nvn_objects,nvn_link=build_nvn_objects(args.compiler,reference,output.parent/'native-objects',args.nvn_backend) if args.nvn_backend else ([],[])
        folder = output/f'round-{iteration:02d}'
        folder.mkdir()
        (folder/'trace.c').write_text(source, encoding='utf-8')
        command = [str(args.compiler.resolve()), 'cc', '-target', 'x86_64-windows-gnu', '-O1','-mcx16',
                   '-I', str(Path(__file__).parent/'runtime'), '-I', str(reference/'runtime'), str(folder/'trace.c'), *(str(o) for o in nvn_objects), *nvn_link, '-lbcrypt', '-luser32', '-o', str(folder/'trace.exe')]
        compile_started=perf_counter()
        try:
            if units is not None:
                build_stats=cached_build(args.compiler,reference,units,source,folder,output.parent/'native-objects',workers=8,extra_objects=[*nvn_objects,*nvn_link])
                compile_result=subprocess.CompletedProcess(command,0,(folder/'compiler.log').read_text(encoding='utf-8'),'')
                print(f'Build cache: {build_stats}',flush=True)
            else:
                compile_result=run_compiler(command)
        except subprocess.TimeoutExpired:
            checkpoint('compile-timeout',translation)
            raise
        except subprocess.CalledProcessError as error:
            checkpoint('compile-failed',translation,error.returncode)
            raise
        compile_seconds=perf_counter()-compile_started
        (folder/'compiler.log').write_text(compile_result.stdout+compile_result.stderr, encoding='utf-8')
        if compile_result.returncode:
            checkpoint('compile-failed',translation,compile_result.returncode)
        compile_result.check_returncode()
        if args.build_only:
            if args.nvn_hle:  # build-specific run variables for run_guarded.ps1 (the SDK's base follows main's size)
                hle=';'.join(f'{name}={image.symbols[name][0]:#x}' for name in HLE_SYMBOLS if name in image.symbols)
                (output/'run-env.txt').write_text(f"AC_NVN_HLE_BOOTSTRAP={image.symbols['nvnBootstrapLoader'][0]:#x}\n"
                                                  f"AC_HLE_SYMBOLS={hle}\n",encoding='utf-8')
            print(f'Built {folder/"trace.exe"} (compile {compile_seconds:.1f}s); not run',flush=True)
            checkpoint('built',translation)
            return
        checkpoint('running',translation)
        run_started=perf_counter()
        try:
            run_env=os.environ.copy()
            run_env.pop('AC_EXPERIMENTAL_DRAW_PACKAGE',None)
            if args.experimental_vulkan_package:
                run_env['AC_EXPERIMENTAL_DRAW_PACKAGE']=str(args.experimental_vulkan_package.resolve())
            run_env.pop('AC_NVN_HLE_BOOTSTRAP',None)
            run_env.pop('AC_NVN_HLE_LINKED',None)
            for name in ('AC_HLE_SYMBOLS','AC_FONT','AC_FONT_EXT'):
                run_env.pop(name,None)
            if args.nvn_hle:
                run_env['AC_NVN_HLE_BOOTSTRAP']=hex(image.symbols['nvnBootstrapLoader'][0])
                if args.nvn_backend:
                    run_env['AC_NVN_HLE_LINKED']=args.nvn_backend
                if args.nvn_backend=='vulkan':
                    run_env['AC_GLSLANG']=str(GLSLANG)
                run_env['AC_HLE_SYMBOLS']=';'.join(f'{name}={image.symbols[name][0]:#x}' for name in HLE_SYMBOLS if name in image.symbols)
                fonts=Path(__file__).resolve().parent.parent/'userdata/fonts'
                for variable,stems in (('AC_FONT',('nintendo_udsg-r_std_003','nintendo_udsgr_std_003')),('AC_FONT_EXT',('nintendo_ext_003',))):
                    found=[fonts/f'{stem}{suffix}' for stem in stems for suffix in ('.bfttf','.ttf') if (fonts/f'{stem}{suffix}').is_file()]
                    if found:
                        run_env[variable]=str(found[0])
            if args.unbounded:
                with (folder/'trace.log').open('w',encoding='utf-8',errors='replace') as log:
                    played=subprocess.run([str(folder/'trace.exe'), str(folder/'relocations.bin')], cwd=output, stdout=log, stderr=subprocess.STDOUT, timeout=args.run_timeout,env=run_env)
                lines=(folder/'trace.log').read_text(encoding='utf-8',errors='replace').splitlines()
                stop=next((l for l in reversed(lines) if l.startswith('STOP ')),lines[-1] if lines else '')
                run=subprocess.CompletedProcess(played.args,played.returncode,stop+'\n','')
            else:
                run = subprocess.run([str(folder/'trace.exe'), str(folder/'relocations.bin')], cwd=output, capture_output=True, text=True, timeout=args.run_timeout,env=run_env)
        except subprocess.TimeoutExpired as error:
            captured=''.join(part.decode('utf-8',errors='replace') if isinstance(part,bytes) else (part or '')
                             for part in (error.stdout,error.stderr))
            if args.unbounded:
                with (folder/'trace.log').open('a',encoding='utf-8') as log:
                    log.write('\nHOST process timeout\n')
            else:
                (folder/'trace.log').write_text(captured+'\nHOST process timeout\n',encoding='utf-8')
            checkpoint('process-timeout',translation)
            raise
        if not args.unbounded:
            (folder/'trace.log').write_text(run.stdout+run.stderr, encoding='utf-8')
        last = run.stdout.splitlines()[-1] if run.stdout else run.stderr
        run_seconds=perf_counter()-run_started
        print(f'Round {iteration} (compile {compile_seconds:.1f}s, run {run_seconds:.1f}s): {last}', flush=True)
        rounds.append({'round': iteration, 'translation': translation, 'exit_code': run.returncode, 'stop': last,
                       'compile_seconds':compile_seconds,'run_seconds':run_seconds})
        checkpoint('stopped',translation,run.returncode)
        match = re.search(r'STOP untranslated pc=(0x[0-9a-f]+)', run.stdout)
        if run.returncode != 20 or not match:
            break
        target = int(match.group(1),16)
        if translation['pending_static_targets']:
            checkpoint('translation-budget-exhausted',translation,run.returncode)
            raise RuntimeError('Instruction translation budget exhausted; increase --instructions and resume from report.json')
        if target in seeds:
            break
        image.locate(target, text_only=True)
        seeds.append(target)
        pointer_observed.add(target)
    checkpoint('stopped',translation,run.returncode)
    if run.returncode not in (0, 20):
        raise RuntimeError(f'Native trace failed: exit {run.returncode}; see trace.log')


if __name__ == '__main__':
    main()
