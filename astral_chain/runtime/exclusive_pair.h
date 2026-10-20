/* Guest execution is serialized. The scheduler clears both pair and scalar
 * exclusive monitors on every context switch. Concurrent DMA is unsupported. */
#include <intrin.h>
static int pair_compare_exchange(uint64_t address,uint64_t low,uint64_t high,uint64_t expected[2]) {
 unsigned __int128 before=((unsigned __int128)expected[1]<<64)|expected[0];
 unsigned __int128 after=((unsigned __int128)high<<64)|low;
 unsigned __int128 found=__sync_val_compare_and_swap((unsigned __int128*)(uintptr_t)address,before,after);
 expected[0]=(uint64_t)found;expected[1]=(uint64_t)(found>>64);
 return found==before;
}
static __thread struct { Cpu *owner; uint64_t address,low,high; unsigned width; int valid; } pair_monitor;  /* per host thread (parallel mode) */
static void rt_clear_pair_monitor(Cpu *c) {
 if(pair_monitor.owner==c) pair_monitor.valid=0;
}
static void pair_memory(uint64_t address,unsigned width) {
 unsigned bytes=width/4;
 Region r=query(address);
 if((address&(bytes-1)) || r.permission!=3 || bytes>r.size-(address-r.base))
  boundary("exclusive-pair-memory",&state,state.pc);
 if(width==64) {
  static int checked;
  if(!checked) {
   int features[4];__cpuid(features,1);
   if(!(features[2]&(1<<13))) boundary("host-needs-cmpxchg16b",&state,state.pc);
   checked=1;
  }
 }
}
static void rt_load_pair(Cpu *c,uint64_t address,unsigned width,unsigned first,unsigned second) {
 pair_memory(address,width);
 uint64_t low,high;
 if(width==64) {
  uint64_t value[2]={0,0};
  pair_compare_exchange(address,0,0,value);
  low=(uint64_t)value[0];high=(uint64_t)value[1];
 } else {
  uint64_t value=(uint64_t)_InterlockedCompareExchange64((volatile long long*)(uintptr_t)address,0,0);
  low=(uint32_t)value;high=value>>32;
 }
 pair_monitor.owner=c;pair_monitor.address=address;pair_monitor.low=low;pair_monitor.high=high;
 pair_monitor.width=width;pair_monitor.valid=1;c->excl_addr=~0ull;
 if(first!=31) c->x[first]=low;
 if(second!=31) c->x[second]=high;
}
static uint32_t rt_store_pair(Cpu *c,uint64_t address,unsigned width,uint64_t low,uint64_t high) {
 pair_memory(address,width);
 int valid=pair_monitor.valid && pair_monitor.owner==c && pair_monitor.address==address && pair_monitor.width==width;
 rt_clear_pair_monitor(c);c->excl_addr=~0ull;
 if(!valid) return 1;
 if(width==64) {
  uint64_t expected[2]={pair_monitor.low,pair_monitor.high};
  return !pair_compare_exchange(address,low,high,expected);
 }
 uint64_t expected=pair_monitor.low|(pair_monitor.high<<32);
 uint64_t value=(uint32_t)low|((uint64_t)(uint32_t)high<<32);
 return (uint64_t)_InterlockedCompareExchange64((volatile long long*)(uintptr_t)address,(long long)value,(long long)expected)!=expected;
}
