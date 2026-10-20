"""Project-owned instruction extensions; the pinned upstream lifter is unchanged."""


def lift(lifter, word, pc):
    from lifter import Insn, Unsupported
    if (word>>24)&63==8:
        size,o2,load,pair=(word>>30)&3,(word>>23)&1,(word>>22)&1,(word>>21)&1
        status,second,base,first=(word>>16)&31,(word>>10)&31,(word>>5)&31,word&31
        if pair and not o2:
            if size<2 or (load and (status!=31 or first==second)):
                raise Unsupported('reserved/unpredictable exclusive pair')
            if not load and status!=31 and (status in (first,second) or (base!=31 and status==base)):
                raise Unsupported('overlapping exclusive status register')
            address='c->sp' if base==31 else f'c->x[{base}]'
            width=32 if size==2 else 64
            if load:
                return Insn('op',f'rt_load_pair(c,{address},{width},{first},{second});')
            low='0' if first==31 else f'c->x[{first}]'
            high='0' if second==31 else f'c->x[{second}]'
            assign='' if status==31 else f'c->x[{status}]=status;'
            return Insn('op',f'{{ uint32_t status=rt_store_pair(c,{address},{width},{low},{high}); {assign} }}')
        if not pair and not o2:
            result=lifter.lift(word,pc)
            return Insn('op','rt_clear_pair_monitor(c); '+result.code)
    if word&0xfffff0ff==0xd503305f: # CLREX, all immediate variants.
        return Insn('op','rt_clear_pair_monitor(c); c->excl_addr=~0ull;')
    return lifter.lift(word,pc)
