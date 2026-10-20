"""Direct C labels within bounded chunks, retaining explicit guest control flow."""

# Limit each optimizer control-flow graph to at most 64 guest instructions.
# Full 4 KiB pages caused excessive compile times on large startup slices.
CHUNK_SHIFT=8


def emit_blocks(nodes, limit, stop_at=None):
    pages={}
    for pc in sorted(nodes):
        pages.setdefault(pc>>CHUNK_SHIFT,[]).append(pc)
    parts=['static uint64_t executed_steps;']
    for page,addresses in sorted(pages.items()):
        def jump(target):
            transfer=f'goto L_{target:x};' if target>>CHUNK_SHIFT==page and target in nodes else 'return;'
            return f'c->pc={target}ull; {transfer}'

        parts.append(f'static __attribute__((noinline)) void page_{page:x}(Cpu *c) {{\nswitch(c->pc) {{')
        parts.extend(f'case {pc}ull: goto L_{pc:x};' for pc in addresses)
        parts.append('default: boundary("untranslated",c,c->pc);\n}')
        for pc in addresses:
            word,op=nodes[pc]
            parts.append(f'L_{pc:x}: {{ c->pc={pc}ull;')
            if pc==stop_at:
                parts.append('boundary("pc-test-boundary",c,c->pc);')
            parts.append(f'if(thread_reschedule || (executed_steps & 4095)==0) {{if(thread_checkpoint(c,executed_steps)) return;}}\nif(executed_steps++>={limit}ull) boundary("instruction-budget",c,c->pc);')
            if op is None:
                parts.append('boundary("unsupported",c,c->pc);')
            elif op.kind=='op':
                parts.append(op.code+' '+jump(pc+4))
            elif op.kind in ('b','bl'):
                if op.kind=='bl':
                    parts.append(f'c->x[30]={pc+4}ull;trace_call({pc}ull,{op.target}ull);')
                parts.append(jump(op.target))
            elif op.kind=='bcond':
                parts.append(f'if({op.cond}) {{ {jump(op.target)} }} else {{ {jump(pc+4)} }}')
            elif op.kind in ('br','blr'):
                parts.append(f'uint64_t target={op.target};')
                if op.kind=='blr':
                    parts.append(f'c->x[30]={pc+4}ull;trace_call({pc}ull,target);')
                parts.append('c->pc=target;return;')
            elif op.kind=='ret':
                parts.append(f'c->pc=c->x[{(word>>5)&31}];return;')
            elif op.kind=='trap':
                parts.append(op.code+' boundary("trap-returned",c,c->pc);')
            else:
                parts.append('boundary("unsupported",c,c->pc);')
            parts.append('}')
        parts.append('}')
    parts.append('static void execute(Cpu *c) { for(;;) { if(thread_reschedule) thread_checkpoint(c,executed_steps);')
    if stop_at is not None:
        parts.append(f'if(c->pc=={stop_at}ull) boundary("pc-test-boundary",c,c->pc);')
    parts.append(f'if(c->pc==0x7fffffffffffull) {{puts("RETURN");return;}} switch(c->pc>>{CHUNK_SHIFT}) {{')
    parts.extend(f'case {page}ull: page_{page:x}(c);break;' for page in sorted(pages))
    parts.append('default: boundary("untranslated",c,c->pc); } } }')
    return '\n'.join(parts)
