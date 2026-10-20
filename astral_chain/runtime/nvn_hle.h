/* Opt-in NVN API interception (AC_NVN_HLE_BOOTSTRAP=<guest address>).
 * NVN functions are resolved by name through nvnBootstrapLoader and
 * nvnDeviceGetProcAddress. Each requested name receives a host thunk outside
 * the 39-bit guest address space. Thunks without a host implementation stop
 * explicitly with their name; nothing is acknowledged with invented success.
 * Disabled unless the variable is set, so the GPU command path is unchanged. */
#pragma once
#define NVN_HLE_BASE 0x7ffe00000000ull
#define NVN_HLE_STRIDE 16ull
#define NVN_HLE_MAX 4096u
#define NVN_HLE_NAME 128u
typedef void (*NvnHostFunction)(Cpu *);
static struct {char name[NVN_HLE_NAME];NvnHostFunction host;uint64_t calls;} nvn_thunks[NVN_HLE_MAX];
static unsigned nvn_thunk_count;
static uint64_t nvn_bootstrap_address;
static int nvn_hle_configured;
static uint64_t nvn_lookups;
static int nvn_linked; /* AC_NVN_HLE_LINKED: vendored host implementations present */
static void nvn_device_get_proc_address(Cpu *c);
static const struct {const char *name;NvnHostFunction host;} nvn_host_table[]={
 {"nvnDeviceGetProcAddress",nvn_device_get_proc_address},
};
/* Vendored LASRecomp NVN implementations (runtime/las) register through COFF
 * section .hle$m. These markers sort before and after it within .hle. */
typedef struct {const char *name;NvnHostFunction fn;} NvnLinkedEntry;
__attribute__((used,section(".hle$a"))) static const NvnLinkedEntry nvn_linked_first={0,0};
__attribute__((used,section(".hle$z"))) static const NvnLinkedEntry nvn_linked_last={0,0};
static NvnHostFunction nvn_linked_lookup(const char *name) {
 for(const NvnLinkedEntry *e=&nvn_linked_first+1;e<&nvn_linked_last;e++)
  if(e->name && e->fn && !strcmp(e->name,name))return e->fn;
 return NULL;
}
/* Bounded NUL-terminated guest string inside one readable region. */
static int nvn_guest_name(uint64_t address,char *out) {
 if(!address)return 0;
 Region r=query(address);
 if(!(r.permission&1) || address<r.base || address-r.base>=r.size)return 0;
 uint64_t available=r.size-(address-r.base);
 for(unsigned i=0;i<NVN_HLE_NAME;i++) {
  if(i>=available)return 0;
  char ch=((const char*)(uintptr_t)address)[i];out[i]=ch;
  if(!ch)return i>0;
 }
 return 0;
}
/* Returns 0 (null proc) only for an unreadable or empty name; the caller sees
 * NVN's documented null result for unknown names in that case. */
static uint64_t nvn_register(const char *name) {
 for(unsigned i=0;i<nvn_thunk_count;i++)if(!strcmp(nvn_thunks[i].name,name))return NVN_HLE_BASE+i*NVN_HLE_STRIDE;
 if(nvn_thunk_count>=NVN_HLE_MAX)boundary("nvn-thunk-table-exhausted",&state,state.pc);
 unsigned index=nvn_thunk_count++;
 memcpy(nvn_thunks[index].name,name,strlen(name)+1);
 for(unsigned i=0;i<sizeof(nvn_host_table)/sizeof(nvn_host_table[0]);i++)
  if(!strcmp(nvn_host_table[i].name,name))nvn_thunks[index].host=nvn_host_table[i].host;
 if(!nvn_thunks[index].host && nvn_linked)nvn_thunks[index].host=nvn_linked_lookup(name);
 return NVN_HLE_BASE+index*NVN_HLE_STRIDE;
}
static uint64_t nvn_lookup(uint64_t name_address,const char *via) {
 char name[NVN_HLE_NAME];
 if(!nvn_guest_name(name_address,name)) {
  printf("NVN lookup via=%s name=<unreadable 0x%llx>\n",via,(unsigned long long)name_address);
  boundary("nvn-lookup-name",&state,state.pc);
 }
 uint64_t thunk=nvn_register(name);nvn_lookups++;
 printf("NVN lookup via=%s name=%s thunk=0x%llx host=%u\n",via,name,(unsigned long long)thunk,
  nvn_thunks[(thunk-NVN_HLE_BASE)/NVN_HLE_STRIDE].host!=NULL);
 return thunk;
}
/* void *nvnDeviceGetProcAddress(const NVNdevice *, const char *) */
static void nvn_device_get_proc_address(Cpu *c) {c->x[0]=nvn_lookup(c->x[1],"device");}
#define HLE_INTERCEPT(c) if(hle_intercept(c)) continue;
typedef struct {uint64_t serial;unsigned index;uint64_t in[4],out,lr;} NvnRecent;
static NvnRecent nvn_recent[64];
static void nvn_dump_recent(void) {
 for(unsigned i=0;i<nvn_thunk_count;i++)if(nvn_thunks[i].calls)
  fprintf(stderr,"NVN_CALLS %s %llu\n",nvn_thunks[i].name,(unsigned long long)nvn_thunks[i].calls);
 uint64_t newest=0;for(unsigned i=0;i<64;i++)if(nvn_recent[i].serial>newest)newest=nvn_recent[i].serial;
 for(uint64_t n=newest>=64?newest-63:1;n && n<=newest;n++) {
  NvnRecent *r=&nvn_recent[n%64];if(r->serial!=n)continue;
  fprintf(stderr,"NVN_RECENT %llu %s x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx -> 0x%llx lr=0x%llx\n",(unsigned long long)n,nvn_thunks[r->index].name,
   (unsigned long long)r->in[0],(unsigned long long)r->in[1],(unsigned long long)r->in[2],(unsigned long long)r->in[3],(unsigned long long)r->out,(unsigned long long)r->lr);
 }
}
#include "pl_hle.h"
/* SDK exports replaced by host functions. native_trace resolves the names it
 * supports and passes AC_HLE_SYMBOLS="name=0xaddress;..."; unknown names stop. */
static const struct {const char *name;NvnHostFunction fn;} hle_symbol_table[]={
 {"_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE",pl_get_shared_font},
 {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE",pl_get_shared_font_address},
 {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE",pl_get_shared_font_size},
};
static struct {uint64_t address;NvnHostFunction fn;} hle_overrides[16];
static unsigned hle_override_count;
static void hle_symbols_configure(Cpu *c) {
 static char list[4096];DWORD n=GetEnvironmentVariableA("AC_HLE_SYMBOLS",list,sizeof(list));
 if(!n) return;
 if(n>=sizeof(list)) boundary("hle-symbols-configuration",c,c->pc);
 for(char *item=strtok(list,";");item;item=strtok(NULL,";")) {
  char *equals=strchr(item,'=');if(!equals) boundary("hle-symbols-configuration",c,c->pc);
  *equals=0;char *end;unsigned long long address=strtoull(equals+1,&end,0);
  if(*end || !address || hle_override_count==16) boundary("hle-symbols-configuration",c,c->pc);
  unsigned found=0;
  for(unsigned i=0;i<sizeof(hle_symbol_table)/sizeof(hle_symbol_table[0]);i++) if(!strcmp(hle_symbol_table[i].name,item)) {
   hle_overrides[hle_override_count].address=address;hle_overrides[hle_override_count++].fn=hle_symbol_table[i].fn;found=1;
  }
  if(!found) {printf("HLE unknown symbol %s\n",item);boundary("hle-symbol-unknown",c,c->pc);}
  printf("HLE symbol %s=0x%llx\n",item,address);
 }
}
static void prof_start(void);
static void nvn_timer_resolution(void);
static int hle_intercept_body(Cpu *c) {
 if(!nvn_hle_configured) {
  nvn_hle_configured=1;
  hle_symbols_configure(c);prof_start();
  if(GetEnvironmentVariableA("AC_NVN_HLE_BOOTSTRAP",NULL,0)) nvn_timer_resolution();
  char value[32];DWORD n=GetEnvironmentVariableA("AC_NVN_HLE_BOOTSTRAP",value,sizeof(value));
  if(n && n<sizeof(value)) {
   char *end;unsigned long long address=strtoull(value,&end,0);
   if(*end || !address)boundary("nvn-hle-configuration",c,c->pc);
   nvn_bootstrap_address=address;
   nvn_linked=GetEnvironmentVariableA("AC_NVN_HLE_LINKED",NULL,0)!=0;
   printf("NVN HLE enabled bootstrap=0x%llx linked=%d guest_host_thread=%lu\n",address,nvn_linked,GetCurrentThreadId());
  }
 }
 for(unsigned i=0;i<hle_override_count;i++) if(c->pc==hle_overrides[i].address) {
  uint64_t return_address=c->x[30];hle_overrides[i].fn(c);c->pc=return_address;return 1;
 }
 if(!nvn_bootstrap_address)return 0;
 if(c->pc==nvn_bootstrap_address) {
  /* void *nvnBootstrapLoader(const char *) */
  c->x[0]=nvn_lookup(c->x[0],"bootstrap");c->pc=c->x[30];return 1;
 }
 if(c->pc<NVN_HLE_BASE || c->pc>=NVN_HLE_BASE+(uint64_t)nvn_thunk_count*NVN_HLE_STRIDE)return 0;
 uint64_t offset=c->pc-NVN_HLE_BASE;
 if(offset%NVN_HLE_STRIDE)boundary("nvn-thunk-misaligned",c,c->pc);
 unsigned index=(unsigned)(offset/NVN_HLE_STRIDE);
 nvn_thunks[index].calls++;
 if(!nvn_thunks[index].host) {
  printf("NVN unimplemented name=%s calls=%llu\n",nvn_thunks[index].name,(unsigned long long)nvn_thunks[index].calls);
  boundary("nvn-unimplemented",c,c->pc);
 }
 uint64_t return_address=c->x[30],in[4]={c->x[0],c->x[1],c->x[2],c->x[3]};
 nvn_thunks[index].host(c);c->pc=return_address;
 /* Bounded call trace: the first 4096 host calls, then every 65536th. */
 static uint64_t traced;traced++;
 nvn_recent[traced%64]=(NvnRecent){traced,index,{in[0],in[1],in[2],in[3]},c->x[0],return_address};
 if(traced<=4096 || !(traced&65535))
  printf("NVN call %llu %s x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx -> 0x%llx lr=0x%llx\n",(unsigned long long)traced,nvn_thunks[index].name,
   (unsigned long long)in[0],(unsigned long long)in[1],(unsigned long long)in[2],(unsigned long long)in[3],
   (unsigned long long)c->x[0],(unsigned long long)return_address);
 return 1;
}
/* Runs at every dispatch. The first call (still single-threaded) configures HLE and AC_PARALLEL; in parallel mode
 * HLE targets run under the kernel lock, and a thread they leave waiting (fence wait, present pacing) blocks here. */
/* Parallel mode: NVN calls (command translation, submission: milliseconds each) hold their own recursive lock, not
 * the kernel lock, so other guest threads' supervisor calls are not stuck behind them (run 422). NVN code that
 * touches thread state (fence parking, present pacing) takes the kernel lock inside: order nvn -> kernel only. */
static SRWLOCK nvn_srw=SRWLOCK_INIT;
static volatile DWORD nvn_owner;
static unsigned nvn_depth;
static void nvn_lock(void) {
 DWORD me=GetCurrentThreadId();
 if(nvn_owner==me) {nvn_depth++;return;}
 if(!TryAcquireSRWLockExclusive(&nvn_srw)) {
  LARGE_INTEGER a,b;QueryPerformanceCounter(&a);AcquireSRWLockExclusive(&nvn_srw);QueryPerformanceCounter(&b);
  nvn_contended++;nvn_contended_qpc+=(uint64_t)(b.QuadPart-a.QuadPart);
 }
 nvn_owner=me;nvn_depth=1;
}
static void nvn_unlock(void) {if(--nvn_depth==0) {nvn_owner=0;ReleaseSRWLockExclusive(&nvn_srw);}}
static int hle_intercept(Cpu *c) {
 if(!nvn_hle_configured) {
#ifndef LEAN_PAGES
#define LEAN_PAGES 0
#endif
  /* Lean builds have no cooperative checkpoints in translated code: parallel mode only. */
  parallel_mode=LEAN_PAGES || GetEnvironmentVariableA("AC_PARALLEL",NULL,0)!=0;
  if(parallel_mode) {live_cpu[0]=&state;printf("PARALLEL guest threads on host threads\n");}
  return hle_intercept_body(c);
 }
 if(!parallel_mode) return hle_intercept_body(c);
 uint64_t pc=c->pc;
 int target=nvn_bootstrap_address && (pc==nvn_bootstrap_address ||
            (pc>=NVN_HLE_BASE && pc<NVN_HLE_BASE+(uint64_t)nvn_thunk_count*NVN_HLE_STRIDE));
 int nvn=target;
 for(unsigned i=0;!target && i<hle_override_count;i++) target=pc==hle_overrides[i].address;
 if(!target) return 0;
 static int timed=-1;  /* AC_PARALLEL_STATS: host time per guest thread in NVN calls (1.5 million calls/s) */
 if(timed<0) timed=GetEnvironmentVariableA("AC_PARALLEL_STATS",NULL,0)!=0;
 LARGE_INTEGER t0,t1;
 if(timed) QueryPerformanceCounter(&t0);
 if(nvn) nvn_lock(); else klock();
 int handled=hle_intercept_body(c);
 if(nvn) nvn_unlock(); else kunlock();
 if(timed && nvn && current_thread<33) {
  QueryPerformanceCounter(&t1);nvn_thread_qpc[current_thread]+=(uint64_t)(t1.QuadPart-t0.QuadPart);nvn_thread_calls[current_thread]++;
 }
 if(guest_threads[current_thread].status!=THREAD_RUNNABLE) {klock();parallel_block();kunlock();}
 return handled;
}
/* ---- glue for vendored LASRecomp NVN objects (runtime/las) ---------------- */
#define NVN_GUEST_RETURN 0x7fffffffffffull
/* Set by the generated entry code; absent in runtime-only test builds. */
static void (*guest_execute)(Cpu *c);
static unsigned nvn_nested_guest_calls;
/* Host -> guest callback (command-memory refill). Cooperative thread switches
 * swap Cpu contents on one host stack, so none may occur while nested;
 * guest_threads.h stops explicitly if a switch would be required. */
void guest_call(Cpu *c,uint64_t address) {
 uint64_t pc=c->pc,lr=c->x[30];
 if(nvn_nested_guest_calls>=8)boundary("nvn-guest-call-depth",c,pc);
 c->x[30]=NVN_GUEST_RETURN;c->pc=address;nvn_nested_guest_calls++;thread_switch_locked++;
 if(!guest_execute)boundary("nvn-guest-call-unavailable",c,pc);
 guest_execute(c);
 thread_switch_locked--;nvn_nested_guest_calls--;c->pc=pc;c->x[30]=lr;
}
__attribute__((noreturn)) void nvn_glue_fatal(const char *message) {boundary(message,&state,state.pc);}
uint64_t plat_now_ns(void) {
 static LARGE_INTEGER frequency;LARGE_INTEGER now;
 if(!frequency.QuadPart)QueryPerformanceFrequency(&frequency);
 QueryPerformanceCounter(&now);
 return (uint64_t)((unsigned __int128)now.QuadPart*1000000000u/(uint64_t)frequency.QuadPart);
}
void plat_sleep_until_ns(uint64_t t) {for(uint64_t now=plat_now_ns();now<t;now=plat_now_ns())Sleep(0);}
/* Pages of [addr,addr+size) written by the CPU since the last query of them
 * (LASRecomp platform/windows.c semantics). Only the guest heap is reserved with
 * MEM_WRITE_WATCH; elsewhere -1 tells the backend tracking is unavailable, and it
 * falls back to reading memory in place / comparing contents. If more pages
 * changed than fit, the range is reset and the caller treats it as all written. */
/* One OS write tracker (GetWriteWatch with reset) feeding independent consumers:
 * every harvest stamps written pages with a new generation, and each consumer
 * remembers the generation it last saw per page. Consumers: the backend's
 * write-tracking API (plat_written_pages) and the GPU heap mirror. */
#define TRACK_PAGES (HEAP_LIMIT/4096)
static uint32_t *track_gen,*track_api_seen,*track_mirror_seen,track_clock;
static int track_ready(void) {
 if(!track_gen) {
  track_gen=calloc(TRACK_PAGES,4);track_api_seen=calloc(TRACK_PAGES,4);track_mirror_seen=calloc(TRACK_PAGES,4);
  if(!track_gen || !track_api_seen || !track_mirror_seen) boundary("write-track-allocation",&state,state.pc);
 }
 return 1;
}
static void track_harvest(uint64_t lo,uint64_t hi) {
 static uint64_t written[65536];
 track_ready();uint32_t g=++track_clock;
 for(uint64_t a=lo;a<hi;) {
  uint64_t e=hi-a>(256ull<<20) ? a+(256ull<<20) : hi; /* 65536 pages: the buffer never overflows */
  ULONG_PTR n=65536;ULONG granularity=4096;
  if(GetWriteWatch(WRITE_WATCH_FLAG_RESET,(void*)(uintptr_t)a,(SIZE_T)(e-a),(PVOID*)written,&n,&granularity))
   boundary("write-track-query",&state,state.pc);
  for(ULONG_PTR i=0;i<n;i++) track_gen[(written[i]-heap_base)>>12]=g;
  a=e;
 }
}
int plat_written_pages(uint64_t addr,uint64_t size,uint64_t *pages,int max,uint64_t *page) {
 if(max<=0 || addr<heap_base || size>HEAP_LIMIT || addr-heap_base>HEAP_LIMIT-size) return -1;
 uint64_t lo=addr&~4095ull,hi=(addr+size+4095)&~4095ull;
 if(hi-heap_base>HEAP_LIMIT) hi=heap_base+HEAP_LIMIT;
 track_harvest(lo,hi);*page=4096;int n=0;
 for(uint64_t a=lo;a<hi;a+=4096) {
  uint64_t i=(a-heap_base)>>12;
  if(track_gen[i]!=track_api_seen[i]) {track_api_seen[i]=track_gen[i];if(n<max) pages[n]=a;n++;}
 }
 return n>max ? max : n; /* n == max: the caller treats the whole range as written */
}
/* GPU heap mirror: only ranges the GPU reads are kept current (bound storage
 * buffers, vertex/index data read in place). Registering a range forces its
 * pages to be copied once; afterwards only CPU-written pages are copied, and
 * only those ranges are scanned for writes. */
static uint8_t *track_interest;
static struct {uint64_t lo,hi;} track_ranges[8192];
static unsigned track_nranges;
void nvn_heap_mirror_interest(uint64_t addr,uint64_t size) {
 if(!size || addr<heap_base || addr-heap_base>=HEAP_LIMIT) return;
 track_ready();
 if(!track_interest) {track_interest=calloc(TRACK_PAGES,1);if(!track_interest) boundary("mirror-interest-allocation",&state,state.pc);}
 uint64_t lo=(addr-heap_base)>>12,hi=(addr-heap_base+size+4095)>>12;
 if(hi>TRACK_PAGES) hi=TRACK_PAGES;
 uint64_t first=hi;
 for(uint64_t i=lo;i<hi;i++) if(!track_interest[i]) {
  track_interest[i]=1;track_mirror_seen[i]=~track_gen[i]; /* copy once */
  if(first==hi) first=i;
 }
 if(first==hi) return;
 /* The list only grows and stays sorted, with overlapping or adjacent ranges merged (the dirty scan coalesces
  * neighbours into few GetWriteWatch calls). */
 unsigned a=0,b=track_nranges;
 while(a<b) {unsigned m=(a+b)/2;if(track_ranges[m].hi<first) a=m+1; else b=m;}
 if(a<track_nranges && track_ranges[a].lo<=hi) {
  if(first<track_ranges[a].lo) track_ranges[a].lo=first;
  if(hi>track_ranges[a].hi) track_ranges[a].hi=hi;
 } else {
  if(track_nranges==8192) boundary("mirror-interest-ranges",&state,state.pc);
  memmove(&track_ranges[a+1],&track_ranges[a],(track_nranges-a)*sizeof(track_ranges[0]));
  track_ranges[a].lo=first;track_ranges[a].hi=hi;track_nranges++;
 }
 unsigned k=a+1;
 while(k<track_nranges && track_ranges[k].lo<=track_ranges[a].hi) {
  if(track_ranges[k].hi>track_ranges[a].hi) track_ranges[a].hi=track_ranges[k].hi;
  k++;
 }
 if(k>a+1) {memmove(&track_ranges[a+1],&track_ranges[k],(track_nranges-k)*sizeof(track_ranges[0]));track_nranges-=k-a-1;}
}
/* Pages written since the mirror last copied them, within the registered ranges. */
int nvn_heap_mirror_dirty(uint64_t limit,uint64_t *pages,int max) {
 static unsigned range_cursor;static uint64_t page_cursor;
 track_ready();
 /* One GetWriteWatch per span of ranges less than 256 KB apart: a call per range cost ~10% of the main
  * thread (run 434, thousands of small ranges per submission). Gap pages are harvested too; harmless, every
  * consumer compares generations. */
 if(!range_cursor && !page_cursor)
  for(unsigned r=0;r<track_nranges;) {
   uint64_t lo=track_ranges[r].lo,hi=track_ranges[r].hi;r++;
   while(r<track_nranges && track_ranges[r].lo-hi<=64) {hi=track_ranges[r].hi;r++;}
   track_harvest(heap_base+(lo<<12),heap_base+(hi<<12));
  }
 int n=0;
 for(unsigned r=range_cursor;r<track_nranges;r++) {
  uint64_t start=r==range_cursor && page_cursor ? page_cursor : track_ranges[r].lo,end=track_ranges[r].hi;
  if(end>limit>>12) end=limit>>12;
  for(uint64_t i=start;i<end;i++) if(track_gen[i]!=track_mirror_seen[i]) {
   track_mirror_seen[i]=track_gen[i];pages[n++]=heap_base+(i<<12);
   if(n==max) {range_cursor=r;page_cursor=i+1;return n;}
  }
  page_cursor=0;
 }
 range_cursor=0;page_cursor=0;return n;
}
/* A new mirror holds nothing: every registered range is copied again. */
void nvn_heap_mirror_mark_all(uint64_t limit) {
 (void)limit;track_ready();
 for(unsigned r=0;r<track_nranges;r++)
  for(uint64_t i=track_ranges[r].lo;i<track_ranges[r].hi;i++) track_mirror_seen[i]=~track_gen[i];
}
/* After the GPU's results were copied into guest memory: the mirror already holds them. */
void nvn_heap_mirror_clean(uint64_t addr,uint64_t size) {
 uint64_t lo=addr&~4095ull,hi=(addr+size+4095)&~4095ull;
 track_harvest(lo,hi);
 for(uint64_t a=lo;a<hi;a+=4096) {uint64_t i=(a-heap_base)>>12;track_mirror_seen[i]=track_gen[i];}
}
/* Readable committed guest heap memory, without a system call: the heap is
 * committed up to heap_size, and the only no-access pages inside it are the
 * runtime's own stack-map and transfer-memory sources (unless the heap is
 * GPU-pinned, when those stay read-write). */
int nvn_guest_readable(uint64_t addr,uint64_t size) {
 if(addr<heap_base || size>heap_size || addr-heap_base>heap_size-size) return 0;
 if(heap_gpu_pinned) return 1;
 for(unsigned i=0;i<64;i++) if(stack_maps[i].size &&
    addr<stack_maps[i].source+stack_maps[i].size && stack_maps[i].source<addr+size) return 0;
 for(unsigned i=0;i<32;i++) if(transfer_memory[i].size &&
    addr<transfer_memory[i].address+transfer_memory[i].size && transfer_memory[i].address<addr+size) return 0;
 return 1;
}
uint64_t kernel_heap_size(void) {return heap_size;}
uint64_t nvn_heap_base(void) {return heap_base;}
void nvn_host_sleep_ms(uint32_t ms) {Sleep(ms);}
int nvn_host_cpu_count(void) {SYSTEM_INFO info;GetSystemInfo(&info);return (int)info.dwNumberOfProcessors;}
/* The Vulkan backend presents into the runtime's application window. */
void *nvn_host_window(void) {
 app_open();
 SetWindowTextW(app_window,L"Astral Chain (native)");
 RECT r={0,0,1600,900};AdjustWindowRect(&r,WS_OVERLAPPEDWINDOW,FALSE);
 SetWindowPos(app_window,NULL,0,0,r.right-r.left,r.bottom-r.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
 app_pump();
 return app_window;
}
/* Display pacing: 60 Hz vsync grid in rt_ticks (19.2 MHz). Each present waits for
 * the first vsync at least `interval` refreshes after the previous present.
 * AC_PRESENT_INTERVAL overrides the game's interval (experiments, e.g. 1). */
void nvn_present_pace(int interval) {
 static int override=-2;
 if(override==-2) {char v[16];DWORD n=GetEnvironmentVariableA("AC_PRESENT_INTERVAL",v,sizeof(v));override=n && n<sizeof(v) ? atoi(v) : -1;}
 static unsigned presents;
 if(!(++presents%60)) {fflush(stdout);fflush(stderr);} /* long runs end by timeout; keep the log current */
 static int logged_interval=-1;
 if(interval!=logged_interval) {printf("NVN present interval=%d override=%d\n",interval,override);logged_interval=interval;}
 if(override>=0) interval=override;
 /* Cutscenes at 30 FPS, as on the Switch (svc #0xB1 in host_runtime.c.in; AC_CUTSCENE_FPS=60: as gameplay). */
 extern int cutscene_mode;
 if(cutscene_mode && interval>0 && interval<2) interval=2;
 if(interval<=0) return;
 static uint64_t origin,last_vsync;
 const uint64_t period=320000; /* 19.2 MHz / 60 Hz */
 uint64_t now=rt_ticks();
 if(!origin) {origin=now;last_vsync=0;return;}
 /* Adaptive: a frame that missed its slot (last + interval) is shown at once
  * rather than held to the next refresh, which would only add idle time. The cadence is kept: the next frame is due
  * an interval after the missed slot, not after the late present (resynchronized only when more than 4 slots
  * behind). Restarting the cadence at each late frame lost up to a slot each time: 30 FPS cutscenes ran at 29.0. */
 uint64_t due=last_vsync+(uint64_t)interval;
 if(now-origin>due*period) {
  last_vsync=now-origin>(due+4)*period ? (now-origin)/period : due;
  return;
 }
 uint64_t vsync=(now-origin+period-1)/period;
 if(vsync<due) vsync=due;
 last_vsync=vsync;
 uint64_t target=origin+vsync*period;
 if(target<=now) return;
 klock();threads_initialize();
 unsigned others=0;
 for(unsigned i=0;i<33;i++) if(i!=current_thread && guest_threads[i].status!=THREAD_UNUSED &&
    guest_threads[i].status!=THREAD_CREATED && guest_threads[i].status!=THREAD_EXITED) others++;
 if(!others) {kunlock();while(rt_ticks()<target) Sleep(0);return;}
 guest_threads[current_thread].wake_tick=target;
 guest_threads[current_thread].status=THREAD_SLEEPING;
 (parallel_mode ? 0 : (thread_reschedule=1));
 kunlock();
}
/* Diagnostic: regions of the guest heap that are not committed PAGE_READWRITE
 * (such pages prevent pinning the heap for Vulkan host-memory import). */
void nvn_report_heap_protection(uint64_t size) {
 unsigned shown=0;
 for(uint64_t a=heap_base;a<heap_base+size && shown<40;) {
  MEMORY_BASIC_INFORMATION m;if(!VirtualQuery((void*)(uintptr_t)a,&m,sizeof(m))) break;
  uint64_t end=(uint64_t)(uintptr_t)m.BaseAddress+m.RegionSize;
  if(m.State!=MEM_COMMIT || m.Protect!=PAGE_READWRITE) {
   fprintf(stderr,"HEAP region 0x%llx-0x%llx state=0x%lx protect=0x%lx\n",(unsigned long long)a,(unsigned long long)end,m.State,m.Protect);shown++;
  }
  a=end;
 }
}
/* Vulkan host-memory import pins every page, so pages the runtime made
 * PAGE_NOACCESS (stack and transfer-memory sources) block it. Around the import
 * those pages are briefly made read-write, then restored: pinned physical pages
 * stay locked for the GPU while the guest keeps Horizon's no-access semantics.
 * Guest threads cannot run during the import (single host thread). */
static struct {uint64_t address,size;DWORD protect;} nvn_import_saved[512];
static unsigned nvn_import_saved_count;
int nvn_heap_prepare_import(uint64_t size) {
 nvn_import_saved_count=0;
 for(uint64_t a=heap_base;a<heap_base+size;) {
  MEMORY_BASIC_INFORMATION m;if(!VirtualQuery((void*)(uintptr_t)a,&m,sizeof(m))) return 0;
  uint64_t end=(uint64_t)(uintptr_t)m.BaseAddress+m.RegionSize;if(end>heap_base+size) end=heap_base+size;
  if(m.State!=MEM_COMMIT) return 0; /* reserved holes cannot be pinned */
  if(m.Protect!=PAGE_READWRITE) {
   DWORD previous;
   if(nvn_import_saved_count==512 || !VirtualProtect((void*)(uintptr_t)a,(SIZE_T)(end-a),PAGE_READWRITE,&previous)) return 0;
   nvn_import_saved[nvn_import_saved_count].address=a;nvn_import_saved[nvn_import_saved_count].size=end-a;
   nvn_import_saved[nvn_import_saved_count++].protect=previous;
  }
  a=end;
 }
 return 1;
}
void nvn_heap_finish_import(int imported) {
 /* Restoring PAGE_NOACCESS on pinned pages makes the GPU fault (device lost,
  * run 317 versus 318): after a successful import they stay read-write and
  * stack/transfer mappings stop protecting heap sources (heap_gpu_pinned). */
 if(imported) {heap_gpu_pinned=1;nvn_import_saved_count=0;return;}
 for(unsigned i=0;i<nvn_import_saved_count;i++) {
  DWORD previous;
  if(!VirtualProtect((void*)(uintptr_t)nvn_import_saved[i].address,(SIZE_T)nvn_import_saved[i].size,nvn_import_saved[i].protect,&previous))
   boundary("heap-import-restore",&state,state.pc);
 }
 nvn_import_saved_count=0;
}
/* AC_PROFILE=1: sample the guest host thread's instruction pointer about every
 * millisecond; every 10 s write profile-<n>.txt (address count, most frequent
 * first) in the run directory for offline symbolization (pdb). */
#define PROF_SLOTS 65536
static struct {uint64_t rip,count;} prof_table[PROF_SLOTS];
/* (sample pc, its caller) pairs: one frame of x64 unwind, to attribute library leaves such as memcpy. */
static struct {uint64_t rip,caller,count;} prof_calls[PROF_SLOTS];
static uint64_t prof_stack_low,prof_stack_high,prof_image_low,prof_image_high;
static HANDLE prof_target;
static uint64_t prof_thread_samples[34];
/* While idle: which guest threads wait, in which state, at which guest pc/lr. */
static struct {unsigned thread,status;uint64_t pc,lr,key,count;} prof_waits[256]; /* per guest thread; [33]: idle in the scheduler */
static DWORD WINAPI prof_thread(LPVOID unused) {
 (void)unused;
 uint64_t last_dump=GetTickCount64();unsigned dumps=0;
 for(;;) {
  Sleep(1);
  if(SuspendThread(prof_target)==(DWORD)-1) continue;
  CONTEXT ctx;ctx.ContextFlags=CONTEXT_CONTROL;
  if(GetThreadContext(prof_target,&ctx)) {
   if(thread_idle_flag) for(unsigned t=0;t<33;t++) {
    unsigned st=guest_threads[t].status;
    if(st<THREAD_WAIT_MUTEX || st==THREAD_EXITED) continue;
    const Cpu *g=t==current_thread ? &state : &guest_threads[t].cpu;
    uint64_t key=st==THREAD_WAIT_GPU ? guest_threads[t].gpu_seq : st==THREAD_WAIT_OBJECTS ? guest_threads[t].wait_handles[0] :
                 st==THREAD_WAIT_CONDITION ? guest_threads[t].condition_key : st==THREAD_WAIT_ADDRESS ? guest_threads[t].address_key :
                 st==THREAD_WAIT_MUTEX ? guest_threads[t].mutex : guest_threads[t].wake_tick;
    unsigned j=0;
    for(;j<256 && prof_waits[j].count;j++) if(prof_waits[j].thread==t && prof_waits[j].status==st && prof_waits[j].pc==g->pc) break;
    if(j<256) {prof_waits[j].thread=t;prof_waits[j].status=st;prof_waits[j].pc=g->pc;prof_waits[j].lr=g->x[30];prof_waits[j].key=key;prof_waits[j].count++;}
   }
   prof_thread_samples[thread_idle_flag ? 33 : current_thread<33 ? current_thread : 33]++;
   uint64_t rip=ctx.Rip,h=(rip*0x9E3779B97F4A7C15ull)>>48;
   for(unsigned i=0;i<64;i++) {unsigned s=(unsigned)((h+i)&(PROF_SLOTS-1));
    if(prof_table[s].rip==rip || !prof_table[s].rip) {prof_table[s].rip=rip;prof_table[s].count++;break;}}
   /* Caller: the first of the top 16 stack slots that points into this executable's image. Exact for leaves
    * such as memcpy (RtlVirtualUnwind on the suspended thread faulted inside ntdll: run 412). Reads stay
    * within the sampled thread's stack. */
   {uint64_t caller=0;
    if(ctx.Rsp>=prof_stack_low && ctx.Rsp<prof_stack_high)
     for(uint64_t a=ctx.Rsp;a+8<=prof_stack_high && a<ctx.Rsp+128;a+=8) {
      uint64_t v=*(const uint64_t *)(uintptr_t)a;
      if(v>=prof_image_low && v<prof_image_high) {caller=v;break;}
     }
    uint64_t k=(rip^caller*0x9E3779B97F4A7C15ull)*0xC2B2AE3D27D4EB4Full>>48;
    for(unsigned i=0;i<64;i++) {unsigned s=(unsigned)((k+i)&(PROF_SLOTS-1));
     if((prof_calls[s].rip==rip && prof_calls[s].caller==caller) || !prof_calls[s].count) {prof_calls[s].rip=rip;prof_calls[s].caller=caller;prof_calls[s].count++;break;}}}
  }
  ResumeThread(prof_target);
  if(GetTickCount64()-last_dump>=10000) {
   last_dump=GetTickCount64();char name[64];snprintf(name,sizeof(name),"profile-%02u.txt",dumps++);
   FILE *f=fopen(name,"w");
   if(f) {
    HMODULE exe=GetModuleHandleA(NULL);fprintf(f,"base 0x%llx\n",(unsigned long long)(uintptr_t)exe);
    for(unsigned i=0;i<PROF_SLOTS;i++) if(prof_table[i].count) {
     HMODULE m=NULL;char path[MAX_PATH]="?";
     if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)(uintptr_t)prof_table[i].rip,&m))
      GetModuleFileNameA(m,path,sizeof(path));
     const char *leaf=strrchr(path,'\\');
     fprintf(f,"0x%llx %llu %s\n",(unsigned long long)prof_table[i].rip,(unsigned long long)prof_table[i].count,leaf?leaf+1:path);
    }
    for(unsigned i=0;i<PROF_SLOTS;i++) if(prof_calls[i].count)
     fprintf(f,"call 0x%llx 0x%llx %llu\n",(unsigned long long)prof_calls[i].rip,(unsigned long long)prof_calls[i].caller,(unsigned long long)prof_calls[i].count);
    for(unsigned j=0;j<256 && prof_waits[j].count;j++) fprintf(f,"wait thread=%u status=%u pc=0x%llx lr=0x%llx key=0x%llx samples=%llu\n",
     prof_waits[j].thread,prof_waits[j].status,(unsigned long long)prof_waits[j].pc,(unsigned long long)prof_waits[j].lr,
     (unsigned long long)prof_waits[j].key,(unsigned long long)prof_waits[j].count);
    for(unsigned t=0;t<34;t++) if(prof_thread_samples[t]) fprintf(f,"thread %u %llu\n",t,(unsigned long long)prof_thread_samples[t]);
    fclose(f);
   }
   memset(prof_table,0,sizeof(prof_table));memset(prof_calls,0,sizeof(prof_calls));memset(prof_thread_samples,0,sizeof(prof_thread_samples));memset(prof_waits,0,sizeof(prof_waits));
  }
 }
}
/* AC_PROFILE_THREAD=N (parallel mode): sample guest thread N's host thread instead of thread 0's. */
static void prof_attach(unsigned thread) {
 static int wanted=-2;
 if(wanted==-2) {char v[16];DWORD n=GetEnvironmentVariableA("AC_PROFILE_THREAD",v,sizeof(v));wanted=n && n<sizeof(v) ? atoi(v) : 0;}
 if((int)thread!=wanted || !GetEnvironmentVariableA("AC_PROFILE",NULL,0)) return;
 if(!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&prof_target,THREAD_ALL_ACCESS,FALSE,0)) return;
 {ULONG_PTR low,high;GetCurrentThreadStackLimits(&low,&high);prof_stack_low=low;prof_stack_high=high;
  const uint8_t *image=(const uint8_t *)GetModuleHandleA(NULL);
  const IMAGE_NT_HEADERS64 *nt=(const IMAGE_NT_HEADERS64 *)(image+((const IMAGE_DOS_HEADER *)image)->e_lfanew);
  /* The executable sections only: data pointers on the stack are not return addresses. */
  const IMAGE_SECTION_HEADER *sec=IMAGE_FIRST_SECTION(nt);
  for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++) if(sec[i].Characteristics&IMAGE_SCN_MEM_EXECUTE) {
   uint64_t lo=(uint64_t)(uintptr_t)image+sec[i].VirtualAddress,hi=lo+sec[i].Misc.VirtualSize;
   if(!prof_image_low || lo<prof_image_low) prof_image_low=lo;
   if(hi>prof_image_high) prof_image_high=hi;
  }}
 HANDLE t=CreateThread(NULL,0,prof_thread,NULL,0,NULL);if(t)CloseHandle(t);
 printf("PROFILE sampling the host thread of guest thread %u\n",thread);
}
static void prof_start(void) {prof_attach(0);}
/* Park the calling guest thread until done(seq) (a GPU submission finished).
 * Returns 0 when no other guest context exists; the caller then blocks. */
int nvn_park_gpu(uint64_t seq,int (*done)(uint64_t)) {
 klock();threads_initialize();
 unsigned others=0;
 for(unsigned i=0;i<33;i++) if(i!=current_thread && guest_threads[i].status!=THREAD_UNUSED &&
    guest_threads[i].status!=THREAD_CREATED && guest_threads[i].status!=THREAD_EXITED) others++;
 if(!others || thread_switch_locked) {kunlock();return 0;}
 guest_threads[current_thread].gpu_seq=seq;guest_threads[current_thread].gpu_done=done;
 guest_threads[current_thread].status=THREAD_WAIT_GPU;(parallel_mode ? 0 : (thread_reschedule=1));
 kunlock();return 1;
}
/* 1 ms Windows timer resolution: the scheduler's idle waits and guest sleeps
 * otherwise round up to the 15.6 ms default tick. */
static void nvn_timer_resolution(void) {
 HMODULE winmm=LoadLibraryExW(L"winmm.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
 UINT (WINAPI *begin)(UINT)=winmm ? (UINT (WINAPI*)(UINT))GetProcAddress(winmm,"timeBeginPeriod") : NULL;
 if(begin && !begin(1)) printf("TIMER resolution 1 ms\n");
}
