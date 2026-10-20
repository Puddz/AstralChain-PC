"""Conservative translation hints; never modify the guest's lazy binding."""
from .containers import FormatError


def plt_got(words, branch_pc):
    """Recognize ADRP x16; LDR x17; ADD x16; BR x17, or return None."""
    if len(words)!=4:
        return None
    a,b,c,d=words
    if a&0x9f00001f!=0x90000010 or b&0xffc003ff!=0xf9400211 or c&0xffc003ff!=0x91000210 or d!=0xd61f0220:
        return None
    offset=((b>>10)&4095)*8
    if (c>>10)&4095 != offset:
        return None
    immediate=((a>>29)&3)|(((a>>5)&0x7ffff)<<2)
    if immediate&(1<<20):immediate-=1<<21
    return ((branch_pc-12)&~4095)+immediate*4096+offset


def import_hints(image):
    hints={}
    for module in image.modules:
        for address,kind,symbol,addend in module.nso.relocs:
            if kind!=1026:
                continue
            name,info,section,value,_=module.nso.syms[symbol]
            if section and (info>>4==0 or module.nso.symbol_visibility[symbol]):
                target=value if section==0xfff1 else module.base+value
            elif name in image.symbols:
                target=image.symbols[name][0]
            else:
                continue
            original=module.nso.u64(address)
            if original < module.nso.size: original+=module.base
            hints[module.base+address]=(target+addend,original)
    return hints


def relative_switch(words, branch_pc):
    """Recognize a guarded seven-instruction signed-relative jump table.

    CMP Windex,#limit; B.HI default; ADRP Xbase; ADD Xbase,#offset;
    LDRSW Xtarget,[Xbase,Xindex,LSL #2]; ADD Xtarget,Xtarget,Xbase; BR Xtarget.
    Returns (table address, entry count), never an inferred unbounded extent.
    """
    if len(words)!=7:
        return None
    cmp,guard,page,offset,load,add,branch=words
    if cmp&0xffc0001f!=0x7100001f or guard&0xff00001f!=0x54000008:
        return None
    index=(cmp>>5)&31
    base=page&31
    target=load&31
    if 31 in (index,base,target) or base in (index,target):
        return None
    if page&0x9f000000!=0x90000000 or offset&0xffc003ff!=(0x91000000|(base<<5)|base):
        return None
    if load&0xffe0fc00!=0xb8a07800 or (load>>16)&31!=index or (load>>5)&31!=base:
        return None
    if add&0xffe0fc00!=0x8b000000 or (add>>16)&31!=base or (add>>5)&31!=target or add&31!=target:
        return None
    if branch!=(0xd61f0000|(target<<5)):
        return None
    immediate=((page>>29)&3)|(((page>>5)&0x7ffff)<<2)
    if immediate&(1<<20): immediate-=1<<21
    table=((branch_pc-16)&~4095)+immediate*4096+((offset>>10)&4095)
    return table,((cmp>>10)&4095)+1


def copied_index_relative_switch(words, branch_pc):
    """Recognize CMP/guard; MOV Wcopy,Windex; the ordinary relative dispatch.

    The copy must be an unshifted 32-bit register MOV of exactly the checked
    index. No additional instructions or inferred dataflow are admitted.
    """
    if len(words) != 8:
        return None
    cmp, guard, copy, page, offset, load, add, branch = words
    source = (cmp >> 5) & 31
    index = (load >> 16) & 31
    if 31 in (source, index) or copy != (0x2a0003e0 | (source << 16) | index):
        return None
    # Only the CMP's register changes in the validation pattern. The actual
    # instructions and BR are untouched, and ADRP remains branch_pc - 16.
    checked_copy = (cmp & ~(31 << 5)) | (index << 5)
    return relative_switch([checked_copy, guard, page, offset, load, add, branch], branch_pc)


def hoisted_relative_switch(words, branch_pc):
    """Bounded translation hints with an earlier ADRP/ADD table base.

    Calls may intervene only for ABI callee-saved X19..X28. This is a code
    coverage hint, not a proof of the callee's behavior or runtime target.
    Guest BR still uses the actual register and unexpected targets still stop.
    """
    if not 7 <= len(words) <= 32:
        return None
    cmp,guard,load,add,branch=words[-5:]
    base=(load>>5)&31
    # Validate the dispatch shape before importing/disassembling its prefix.
    fake_page=0x90000000|base
    fake_add=0x91000000|(base<<5)|base
    if relative_switch([cmp,guard,fake_page,fake_add,load,add,branch],branch_pc) is None:
        return None
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM, CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET
    import struct
    decoder=Cs(CS_ARCH_ARM64,CS_MODE_ARM);decoder.detail=True
    start=branch_pc-4*(len(words)-1)
    for index in range(len(words)-7,-1,-1):
        page,offset=words[index:index+2]
        if page&0x9f00001f != 0x90000000|base:
            continue
        layout=relative_switch([cmp,guard,page,offset,load,add,branch],start+index*4+16)
        if layout is None:
            continue
        middle=words[index+2:-5]
        instructions=list(decoder.disasm(struct.pack('<'+'I'*len(middle),*middle),start+(index+2)*4))
        if len(instructions)!=len(middle):
            return None
        for instruction in instructions:
            if instruction.mnemonic in ('udf','brk','hlt','svc','hvc','smc') or instruction.group(CS_GRP_RET):
                return None
            if instruction.group(CS_GRP_CALL) and not 19 <= base <= 28:
                return None
            if instruction.group(CS_GRP_JUMP) and instruction.mnemonic in ('b','br'):
                return None
            _,writes=instruction.regs_access()
            if any(instruction.reg_name(reg) in (f'x{base}',f'w{base}') for reg in writes):
                return None
        return layout
    return None


def switch_targets(module, words, branch_pc):
    """Only admit complete tables in this module's rodata targeting its text."""
    layout=(relative_switch(words[-7:],branch_pc) or
            copied_index_relative_switch(words[-8:],branch_pc) or
            hoisted_relative_switch(words,branch_pc))
    if layout is None:
        return ()
    table,count=layout
    offset=table-module.base
    ro,size=module.nso.rodata
    if offset%4 or offset<ro or offset+count*4>ro+size:
        return ()
    targets=tuple(table+delta for delta in module.nso.unpack('<'+str(count)+'i',offset))
    low=module.base+module.nso.text[0]
    high=low+module.nso.text[1]
    if any(target%4 or not low<=target<high for target in targets):
        return ()
    return tuple(dict.fromkeys(targets))


def initialization_targets(module):
    """Bounded ELF DT_INIT/DT_INIT_ARRAY hints; do not execute or relocate them."""
    nso=module.nso
    targets=[]
    if nso.dyn.get(12):
        targets.append(module.base+nso.dyn[12])
    address,size=nso.dyn.get(25,0),nso.dyn.get(27,0)
    if size:
        if address%8 or size%8 or size>32768 or not any(
                low<=address and address+size<=low+length for low,length in (nso.rodata,nso.data)):
            raise FormatError('Initializer array outside bounded file-backed data')
        relocations={r[0]:r for r in nso.relocs}
        for offset in range(address,address+size,8):
            relocation=relocations.get(offset)
            if relocation:
                if relocation[1]!=1027:
                    raise FormatError('Unsupported initializer relocation hint')
                target=module.base+relocation[3]
            else:
                target=nso.u64(offset)
            targets.append(target)
    low=module.base+nso.text[0]
    high=low+nso.text[1]
    if any(target%4 or not low<=target<high for target in targets):
        raise FormatError('Initializer hint target outside aligned module text')
    return tuple(dict.fromkeys(targets))


CODE_POINTER_RADIUS = 128


def _relocated_code_slots(module):
    """Aligned RELATIVE pointers in file-backed rodata/data into module text."""
    nso=module.nso
    low,length=nso.text
    slots={}
    for offset,kind,symbol,addend in nso.relocs:
        if kind!=1027 or symbol or offset%8 or addend%4 or not low<=addend<low+length:
            continue
        for segment,(start,size) in enumerate((nso.rodata,nso.data)):
            if start<=offset and offset+8<=start+size:
                slots[offset]=(module.base+addend,segment)
                break
    return slots


def relocated_code_targets(module):
    """Complete bounded inventory of relative text-pointer translation hints.

    Being inside executable text is not proof of a function entry. These only
    extend generated coverage and never change runtime dispatch.
    """
    targets={target for target,segment in _relocated_code_slots(module).values()}
    if len(targets)>50000:
        raise FormatError('Relocated code-pointer hint budget exceeded')
    return tuple(sorted(targets))


def adjacent_code_targets(module, observed):
    """Translation hints from nearby RELATIVE code pointers, never execution order.

    Search at most 128 contiguous pointer slots either side of an observed
    target. Invalid entries, gaps and file-backed segment boundaries stop a run.
    """
    slots=_relocated_code_slots(module)
    observed=set(observed)
    targets=set()
    for offset,(target,segment) in slots.items():
        if target not in observed:
            continue
        for direction in (-8,8):
            for distance in range(1,CODE_POINTER_RADIUS+1):
                neighbor=slots.get(offset+direction*distance)
                if neighbor is None or neighbor[1]!=segment:
                    break
                targets.add(neighbor[0])
    targets.difference_update(observed)
    if len(targets)>4096:
        raise FormatError('Adjacent code-pointer hint budget exceeded')
    return tuple(sorted(targets))
