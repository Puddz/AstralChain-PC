/* Keeps Windows' own allocations out of the guest's address space.
 *
 * The game maps memory at addresses it picks itself (its shared memory views at random addresses anywhere in its
 * 512 GB address space, thread stacks and thread-local blocks in the stack region), and the runtime maps the host
 * memory at the same addresses. Windows places this process's own allocations (thread stacks, driver and upscaler
 * buffers) anywhere too, inside that range included. One there could take an address the game wanted: rarely at
 * startup ("host-shared-placeholder-reserve"), and when a save state of an earlier run was loaded, its views had to be
 * mapped again at the addresses that run picked ("shared-map-overlap"; seen at 3840x1080 with DLSS, whose larger
 * buffers make that more likely). So at startup, once the code images, stack, thread-local area and heap have their
 * fixed reservations and before any graphics or sound library loads, every free part of 4 GB .. 512 GB is reserved
 * as a placeholder (address space only, no memory). A guest mapping releases the 64 KB granules it needs first
 * (guest_space_take); when it is undone they are reserved again (guest_space_give). For the guest's bookkeeping
 * (query, host_clip) reserved granules count as free. AC_GUEST_SPACE_GUARD=0 turns it off (test). */
#define GUARD_LO 0x100000000ull
#define GUARD_HI (1ull<<39)
#define GUARD_GRANULES ((GUARD_HI-GUARD_LO)>>16)
static uint8_t *guard_bits;  /* one bit per 64 KB granule of GUARD_LO .. GUARD_HI: reserved by the guard */
typedef PVOID (WINAPI *GuardAlloc2)(HANDLE,PVOID,SIZE_T,ULONG,ULONG,MEM_EXTENDED_PARAMETER*,ULONG);
static GuardAlloc2 guard_alloc2;
static SRWLOCK guard_lock=SRWLOCK_INIT;
static int guard_bit(uint64_t granule) {return (guard_bits[granule>>3]>>(granule&7))&1;}
static void guard_set(uint64_t granule,int on) {
 if(on) guard_bits[granule>>3]|=(uint8_t)(1u<<(granule&7)); else guard_bits[granule>>3]&=(uint8_t)~(1u<<(granule&7));
}
/* 1 if the address is in a granule the guard holds (free for the guest). */
static int guest_space_guarded(uint64_t address) {
 if(!guard_bits || address<GUARD_LO || address>=GUARD_HI) return 0;
 AcquireSRWLockShared(&guard_lock);
 int on=guard_bit((address-GUARD_LO)>>16);
 ReleaseSRWLockShared(&guard_lock);
 return on;
}
static void guest_space_guard(void) {
 char e[8];DWORD n=GetEnvironmentVariableA("AC_GUEST_SPACE_GUARD",e,sizeof(e));
 if(n && n<sizeof(e) && e[0]=='0') return;
 guard_alloc2=(GuardAlloc2)GetProcAddress(GetModuleHandleW(L"KernelBase.dll"),"VirtualAlloc2");
 if(!guard_alloc2) return;
 guard_bits=(uint8_t*)VirtualAlloc(NULL,(SIZE_T)((GUARD_GRANULES+7)/8),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
 if(!guard_bits) return;
 uint64_t reserved=0;unsigned pieces=0,failed=0;
 for(uint64_t p=GUARD_LO;p<GUARD_HI;) {
  MEMORY_BASIC_INFORMATION info;
  if(!VirtualQuery((void*)(uintptr_t)p,&info,sizeof(info))) break;
  uint64_t rb=(uint64_t)(uintptr_t)info.BaseAddress,re=rb+info.RegionSize;
  if(re>GUARD_HI) re=GUARD_HI;
  if(re<=p) break;
  if(info.State==MEM_FREE) {
   uint64_t lo=(p+65535)&~65535ull,hi=re&~65535ull;
   while(lo<hi) {  /* (in pieces of at most 64 GB, smaller where a piece is refused) */
    uint64_t size=hi-lo;if(size>(64ull<<30)) size=64ull<<30;
    while(size>=65536 && guard_alloc2(GetCurrentProcess(),(void*)(uintptr_t)lo,(SIZE_T)size,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0)!=(void*)(uintptr_t)lo)
     size=(size/2)&~65535ull;
    if(size<65536) {failed++;lo+=65536;continue;}
    for(uint64_t g=(lo-GUARD_LO)>>16,end=(lo+size-GUARD_LO)>>16;g<end;g++) guard_set(g,1);
    reserved+=size;pieces++;lo+=size;
   }
  }
  p=re;
 }
 printf("HOST guest address space guard: %.1f GB reserved in %u pieces (%u granules refused)\n",reserved/1073741824.0,pieces,failed);
 fflush(stdout);
}
/* Before the runtime maps or allocates guest memory at [address, address+size): the guard's granules there are
 * released, so the address is free for it (other host allocations there are left alone: the mapping then fails as
 * it did without the guard). */
static void guest_space_take(uint64_t address,uint64_t size) {
 if(!guard_bits || !size) return;
 uint64_t lo=address&~65535ull,hi=(address+size+65535)&~65535ull;
 if(lo<GUARD_LO) lo=GUARD_LO;
 if(hi>GUARD_HI) hi=GUARD_HI;
 AcquireSRWLockExclusive(&guard_lock);
 for(uint64_t p=lo;p<hi;) {
  if(!guard_bit((p-GUARD_LO)>>16)) {p+=65536;continue;}
  uint64_t end=p;
  while(end<hi && guard_bit((end-GUARD_LO)>>16)) end+=65536;
  /* [p, end): guard granules, maybe parts of larger placeholders; each placeholder met is split at the range's ends */
  for(uint64_t q=p;q<end;) {
   MEMORY_BASIC_INFORMATION info;
   if(!VirtualQuery((void*)(uintptr_t)q,&info,sizeof(info)) || info.State!=MEM_RESERVE) {
    guard_set((q-GUARD_LO)>>16,0);q+=65536;continue;  /* (not ours any more) */
   }
   /* (BaseAddress is q's page; AllocationBase the placeholder's start, BaseAddress+RegionSize its end) */
   uint64_t pb=(uint64_t)(uintptr_t)info.AllocationBase,pe=(uint64_t)(uintptr_t)info.BaseAddress+info.RegionSize,cut_end=pe<end ? pe : end;
   if(q!=pb || cut_end!=pe) VirtualFree((void*)(uintptr_t)q,(SIZE_T)(cut_end-q),MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER);
   VirtualFree((void*)(uintptr_t)q,0,MEM_RELEASE);
   for(uint64_t g=q;g<cut_end;g+=65536) guard_set((g-GUARD_LO)>>16,0);
   q=cut_end;
  }
  p=end;
 }
 ReleaseSRWLockExclusive(&guard_lock);
}
/* After the runtime released guest memory at [address, address+size): whole free granules are reserved again. */
static void guest_space_give(uint64_t address,uint64_t size) {
 if(!guard_bits || !size) return;
 uint64_t lo=(address+65535)&~65535ull,hi=(address+size)&~65535ull;
 if(lo<GUARD_LO) lo=GUARD_LO;
 if(hi>GUARD_HI) hi=GUARD_HI;
 AcquireSRWLockExclusive(&guard_lock);
 for(uint64_t p=lo;p<hi;p+=65536) {
  MEMORY_BASIC_INFORMATION info;
  if(guard_bit((p-GUARD_LO)>>16) || !VirtualQuery((void*)(uintptr_t)p,&info,sizeof(info)) || info.State!=MEM_FREE ||
     (uint64_t)(uintptr_t)info.BaseAddress+info.RegionSize<p+65536) continue;
  if(guard_alloc2(GetCurrentProcess(),(void*)(uintptr_t)p,65536,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0)==(void*)(uintptr_t)p)
   guard_set((p-GUARD_LO)>>16,1);
 }
 ReleaseSRWLockExclusive(&guard_lock);
}
