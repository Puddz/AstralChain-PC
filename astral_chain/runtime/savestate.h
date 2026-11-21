/* Save states (experimental; parallel mode only; off unless AC_SAVE_STATES=1).
 *
 * Stopping the world: ss_stop asks every guest thread to park at its next supervisor or HLE call (ss_park, before
 * the call runs: it is run again on resume). A thread already inside a blocking wait (parallel_block_wait) holds
 * the kernel lock whenever it is not asleep, so the coordinator, holding that lock, has it stopped too. The
 * coordinator is the game thread at its per-frame svc #0xB1.
 *
 * Resuming: a host thread cannot return into the frames it was stopped in once the guest state is another one, so
 * it starts the dispatcher again on top of its stack (ss_resume; the old frames below are never returned to). */
/* AC_SAVE_STATES=1 (launcher: Save states): off otherwise - F6 and F7 do nothing and NVN's host structs come from
 * the C heap as before. Also off for the run when the arena's address is taken. */
#include <io.h>
#include <setjmp.h>
static int ss_arena_failed;
static int ss_enabled(void) {
 static int on=-1;
 if(on<0) {const char *e=getenv("AC_SAVE_STATES");on=e && atoi(e)!=0;}
 return on && !ss_arena_failed;
}
typedef struct {
 int valid;
 Cpu cpu[33];
 LONG phase[33];           /* 1 parked before a call (run it again at park_pc), 2 in a wait (continue at after) */
 uint64_t park_pc[33],after[33];
 typeof(guest_threads[0]) threads[33];
 uint64_t steps;
} SsThreads;
static SsThreads ss_saved;
static uint64_t ss_park_pc[33];
static int ss_alive(unsigned i) {
 unsigned s=guest_threads[i].status;
 return s!=THREAD_UNUSED && s!=THREAD_CREATED && s!=THREAD_EXITED;
}
static void ss_park(Cpu *c,uint64_t pc) {
 unsigned i=current_thread;
 (void)c;
 if(thread_switch_locked || i>=33) return;  /* (a nested guest callback finishes first) */
 ss_park_pc[i]=pc;InterlockedExchange(&ss_phase[i],1);
 {const char *w=rt_where("ss_park");
  while(ss_stop && !ss_cmd[i]) Sleep(1);
  rt_where(w);}
 if(ss_cmd[i]) ss_resume(i);
 InterlockedExchange(&ss_phase[i],0);
}
/* The first time a host thread starts over, it does so here, on top of what it was doing; every later time it jumps
 * back to this frame (ss_resume), so loading states over and over does not use up its stack. */
static __thread jmp_buf ss_base;  /* (set with no frame: longjmp then only puts the registers back, it unwinds nothing) */
static __thread int ss_has_base;
static __thread unsigned ss_base_thread;
static __attribute__((noreturn,noinline)) void ss_enter(unsigned i) {
 ss_base_thread=i;
 if(_setjmp(ss_base,NULL)) i=ss_base_thread;
 ss_has_base=1;
 current_thread=i;rt_where_slot=i;
 state=ss_saved.cpu[i];
 thread_switch_locked=0;
 klock();live_cpu[i]=&state;
 InterlockedExchange(&ss_phase[i],0);
 if(ss_saved.phase[i]==2) {
  ss_after=ss_saved.after[i];
  if(guest_threads[i].status!=THREAD_RUNNABLE) parallel_block();
  if(guest_threads[i].status==THREAD_EXITED) {klock_release_all();ExitThread(0);}
  if(ss_saved.after[i]) state.pc=ss_saved.after[i];
 } else state.pc=ss_saved.park_pc[i];
 kunlock();
 guest_execute(&state);
 klock();guest_threads[i].status=THREAD_EXITED;kunlock();
 ExitThread(0);
}
static DWORD WINAPI ss_thread_main(LPVOID parameter) {ss_enter((unsigned)(uintptr_t)parameter);}
static void ss_resume(unsigned i) {
 LONG cmd=InterlockedExchange(&ss_cmd[i],0);
 DWORD me=GetCurrentThreadId();
 if(klock_owner==me) {klock_depth=0;klock_owner=0;ReleaseSRWLockExclusive(&klock_srw);}
 if(nvn_owner==me) {nvn_depth=0;nvn_owner=0;ReleaseSRWLockExclusive(&nvn_srw);}
 if(cmd==2) ExitThread(0);
 if(ss_has_base) {ss_base_thread=i;longjmp(ss_base,1);}
 ss_enter(i);
}
/* Called by the coordinator with the kernel lock held once. Returns with the lock held and every other guest
 * thread stopped, or 0 (lock held, nothing stopped) when they did not all stop in time. */
static int ss_stop_world(void) {
 unsigned self=current_thread;
 LARGE_INTEGER a,b,f;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&a);
 InterlockedExchange(&ss_stop,1);
 for(;;) {
  unsigned running=0,who=99;
  for(unsigned i=0;i<33;i++) if(i!=self && ss_alive(i) && ss_phase[i]==0) {running++;who=i;}
  if(!running) break;
  QueryPerformanceCounter(&b);
  if((b.QuadPart-a.QuadPart)*1000/f.QuadPart>1000) {
   printf("[ss] stop timed out: %u guest threads still running (one is %u, at %s)\n",running,who,rt_where_of[who] ? rt_where_of[who] : "?");
   InterlockedExchange(&ss_stop,0);return 0;
  }
  unsigned depth=klock_release_all();Sleep(1);klock_reacquire(depth);
 }
 QueryPerformanceCounter(&b);
 printf("[ss] world stopped in %.1f ms\n",1000.0*(double)(b.QuadPart-a.QuadPart)/(double)f.QuadPart);
 return 1;
}
static void ss_capture_threads(Cpu *c,uint64_t after) {
 unsigned self=current_thread;
 memset(&ss_saved,0,sizeof(ss_saved));
 for(unsigned i=0;i<33;i++) {
  ss_saved.threads[i]=guest_threads[i];
  if(!ss_alive(i)) continue;
  if(i==self) {ss_saved.cpu[i]=*c;ss_saved.phase[i]=1;ss_saved.park_pc[i]=after;continue;}
  ss_saved.cpu[i]=*live_cpu[i];ss_saved.phase[i]=ss_phase[i];
  ss_saved.park_pc[i]=ss_park_pc[i];ss_saved.after[i]=ss_after_of[i];
 }
 ss_saved.steps=ac_game_steps;ss_saved.valid=1;
}
/* The world is stopped and the tables are those of ss_saved: every guest thread continues from its saved context. */
static __attribute__((noreturn)) void ss_restart_threads(void) {
 unsigned self=current_thread,n=0;
 for(unsigned i=0;i<33;i++) {
  if(i==self) continue;
  int had=ss_phase[i]!=0,want=ss_saved.phase[i]!=0;  /* (a stopped host thread exists; the state has this thread) */
  if(had) {InterlockedExchange(&ss_cmd[i],want ? 1 : 2);n++;}
  else if(want) {
   HANDLE h=CreateThread(NULL,16u<<20,ss_thread_main,(LPVOID)(uintptr_t)i,STACK_SIZE_PARAM_IS_A_RESERVATION,NULL);
   if(!h) boundary("ss-thread-create",&state,state.pc);
   CloseHandle(h);n++;
  }
 }
 InterlockedExchange(&ss_stop,0);
 for(unsigned i=0;i<33;i++) parallel_wake(i);
 printf("[ss] %u guest threads restarted\n",n);fflush(stdout);
 InterlockedExchange(&ss_cmd[self],ss_saved.phase[self] ? 1 : 2);
 ss_resume(self);
}
/* ---- the arena of NVN host structs (las/nvn/nvn.h) ---- */
#define SS_ARENA_BASE 0x6E00000000ull
#define SS_ARENA_SIZE (256ull<<20)
typedef struct { uint64_t magic,top,live; uint64_t free_head[64],free_size[64]; } SsArena;
typedef struct { uint32_t size,type; uint64_t next; } SsBlock;
static SsArena *ss_arena(void) {  /* (NULL: save states are off) */
 static SsArena *a;static int tried;
 if(!tried) {
  tried=1;
  if(ss_enabled()) {
   void *p=VirtualAlloc((void*)(uintptr_t)SS_ARENA_BASE,SS_ARENA_SIZE,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
   if(p==(void*)(uintptr_t)SS_ARENA_BASE) {a=p;a->magic=0x414e455241535341ull;a->top=(sizeof(SsArena)+15)&~15ull;}
   else {
    if(p) VirtualFree(p,0,MEM_RELEASE);
    ss_arena_failed=1;
    printf("[ss] save states are off in this run: the address they need (0x%llx) is taken; start the game again\n",(unsigned long long)SS_ARENA_BASE);
    fflush(stdout);
   }
  }
 }
 return a;
}
void *nvn_obj_alloc(size_t size,int type) {
 SsArena *a=ss_arena();uint64_t need=(size+15)&~15ull;unsigned k;
 if(!a) {void *p=calloc(1,size ? size : 1);if(!p) boundary("nvn-object-memory",&state,state.pc);return p;}
 if(!need) need=16;
 for(k=0;k<64 && a->free_size[k] && a->free_size[k]!=need;k++);
 SsBlock *b;
 if(k<64 && a->free_size[k]==need && a->free_head[k]) {b=(SsBlock*)(uintptr_t)a->free_head[k];a->free_head[k]=b->next;}
 else {
  if(a->top+sizeof(SsBlock)+need>SS_ARENA_SIZE) boundary("ss-arena-full",&state,state.pc);
  b=(SsBlock*)(uintptr_t)(SS_ARENA_BASE+a->top);a->top+=sizeof(SsBlock)+need;
 }
 b->size=(uint32_t)need;b->type=(uint32_t)type;b->next=0;memset(b+1,0,need);a->live++;
 return b+1;
}
void nvn_obj_free(void *p) {
 if(!p) return;
 SsArena *a=ss_arena();SsBlock *b=(SsBlock*)p-1;unsigned k;
 if(!a) {free(p);return;}
 if(!b->type) return;
 for(k=0;k<64 && a->free_size[k] && a->free_size[k]!=b->size;k++);
 b->type=0;a->live--;
 if(k<64) {a->free_size[k]=b->size;b->next=a->free_head[k];a->free_head[k]=(uint64_t)(uintptr_t)b;}
}
size_t nvn_obj_size(void *p) {return ((SsBlock*)p-1)->size;}
void nvn_obj_walk(void (*fn)(void *obj,int type)) {
 SsArena *a=ss_arena();
 if(!a) return;
 for(uint64_t at=(sizeof(SsArena)+15)&~15ull;at<a->top;) {
  SsBlock *b=(SsBlock*)(uintptr_t)(SS_ARENA_BASE+at);
  if(b->type) fn(b+1,(int)b->type);
  at+=sizeof(SsBlock)+b->size;
 }
}
/* ---- one state: in memory, and as a file: savestates\state.acss next to the user's save folder
 * (userdata\saves\<title>\savestates), or AC_SS_FILE ---- */
void vk_ss_quiesce(void);
void vk_ss_drop(int programs);
void *vk_ss_bound_state(size_t *size);
void vk_ss_fix_bound(void);
void nvn_ss_reset_backends(void);
size_t nvn_ss_statics(void *buffer,int set);
typedef struct { uint64_t addr,size,off; } SsRun;
typedef struct { uint8_t kind,perm; uint64_t size; } SsShared;          /* a shared memory handle slot: 1 HID, 2 clock, 3 other */
typedef struct { uint64_t address,size; uint8_t kind,perm; } SsView;
#define SS_NEV 294
/* A state loads where the layout of what it holds is the same: this version of the state code (SS_VERSION: raise it
 * with any change to the tables a state holds) and the same game code (a hash over the start of the main module). A
 * program built again from the same source, also on another PC, takes it. */
#define SS_VERSION 4
static const char *ss_build(void) {
 static char text[32];
 if(!text[0]) {
  uint64_t h=1469598103934665603ull;const uint8_t *code=(const uint8_t*)(uintptr_t)0x7000010000ull;
  for(unsigned k=0;k<(1u<<20);k++) h=(h^code[k])*1099511628211ull;
  snprintf(text,sizeof(text),"v%d-%016llx",SS_VERSION,(unsigned long long)h);
 }
 return text;
}
static struct { SsRun *runs; unsigned n; uint64_t bytes; uint64_t heap_size,heap_base; LONG generation;
 uint8_t *arena; uint64_t arena_top; void *bound; size_t bound_size; uint8_t nvn[256]; size_t nvn_size; uint64_t ticks;
 /* the memory map: these tables are not copied back, the live ones are brought to this layout */
 uint64_t tls[32]; typeof(stack_maps[0]) stacks[64]; typeof(transfer_memory[0]) transfers[32]; uint8_t *transfer_data[32];
 SsShared shared[64]; SsView views[64];
 int16_t ev_obj[SS_NEV]; uint8_t ev_sig[SS_NEV];  /* event handle fields: which kernel object each one names; its state */
 struct { uint64_t size; unsigned refs,has; uint8_t *data; } applet[16];
 uint8_t *hid,*clock; char *thunks; unsigned thunk_count;
 /* sound outputs: the buffers the game had queued (their tags), and those it had not yet been told were played */
 struct { uint32_t active,running,rate,channels,n; uint64_t tags[64]; } audio[16];
 FILE *file; uint64_t session; int same_session;  /* the file being loaded, open at its memory pages */
 DWORD pid; int res_w,res_h; int foreign;  /* foreign: read from a file, made by another process */
} ss_mem;
#define SS_S(x) {&x,sizeof(x),#x,NULL}
typedef struct { void *p; size_t n; const char *name; void *copy; } SsStatic;
static SsStatic ss_statics[]={
 SS_S(next_thread_id),SS_S(next_wait_order),
 SS_S(sessions),SS_S(session_handles),SS_S(session_refs),SS_S(session_peer_closed),SS_S(events),
 SS_S(nvmap_objects),SS_S(nvmap_next_id),SS_S(nv_devices),SS_S(nv_next_fd),
 SS_S(vi_layers),SS_S(vi_next_layer_id),SS_S(vi_next_binder_id),SS_S(vi_display_refs),SS_S(vi_display_layer),
 SS_S(app_messages),SS_S(app_message_head),SS_S(app_message_count),
 SS_S(uncached_ranges),
 /* (only copied back into another process: in the same one the live values carry on) */
 SS_S(hid_sampling),SS_S(hid_touch_sampling),SS_S(keyboard_sample),SS_S(mouse_sample),SS_S(clock_source),SS_S(clock_steady_base),
};
#define SS_NSTATIC (sizeof(ss_statics)/sizeof(ss_statics[0]))
#define SS_NSTATIC_LOCAL (SS_NSTATIC-6)
static HANDLE *ss_ev_field(unsigned k,int *external) {
 *external=0;
 if(k<64) return &events[k].handle;
 k-=64;
 if(k<6) {
  static HANDLE *const global[6]={&app_suspend_event,&library_launch_event,&library_state_event,&app_message_event,&addon_change_event,&interactive_event};
  return global[k];
 }
 k-=6;
 if(k<192) return &nv_devices[k/3].events[k%3];
 k-=192;
 if(k<16) return &vi_layers[k].buffer_event;
 k-=16;
 *external=1;return &audio_outputs[k].wave.completion;  /* (the live sound output is kept: not a copied table) */
}
static int ss_same_object(HANDLE a,HANDLE b) {
 static BOOL (WINAPI *compare)(HANDLE,HANDLE);static int tried;
 if(a==b) return 1;
 if(!tried) {tried=1;compare=(BOOL (WINAPI*)(HANDLE,HANDLE))GetProcAddress(GetModuleHandleW(L"KernelBase.dll"),"CompareObjectHandles");}
 return compare && compare(a,b);
}
/* The writable guest memory as runs of pages the host has readable and writable right now. */
static unsigned ss_guest_runs(SsRun *out,unsigned max,uint64_t *bytes) {
 unsigned n=0;uint64_t total=0;
 for(uint64_t at=0x7000000000ull;at<(1ull<<39);) {
  Region r=query(at);
  if(!r.size) break;
  uint64_t end=r.base+r.size;
  if(r.kind && (r.permission&2)) {
   for(uint64_t p=at;p<end;) {
    MEMORY_BASIC_INFORMATION m;
    if(!VirtualQuery((void*)(uintptr_t)p,&m,sizeof(m))) break;
    uint64_t lo=p,hi=(uint64_t)(uintptr_t)m.BaseAddress+m.RegionSize;
    if(hi>end) hi=end;
    if(m.State==MEM_COMMIT && m.Protect==PAGE_READWRITE) {
     if(n && out[n-1].addr+out[n-1].size==lo) out[n-1].size+=hi-lo;
     else if(n<max) {out[n].addr=lo;out[n].size=hi-lo;n++;}
     total+=hi-lo;
    }
    p=hi;
   }
  }
  at=end;
 }
 *bytes=total;return n;
}
static int ss_stack_saved(unsigned i) {
 for(unsigned j=0;j<64;j++) if(ss_mem.stacks[j].size && ss_mem.stacks[j].size==stack_maps[i].size &&
    ss_mem.stacks[j].source==stack_maps[i].source && ss_mem.stacks[j].destination==stack_maps[i].destination) return 1;
 return 0;
}
/* A shared memory handle slot for this section (a view is mapped and unmapped through one). */
static unsigned ss_shared_slot(HANDLE source,unsigned perm,uint64_t size,int *temporary) {
 *temporary=0;
 for(unsigned i=0;i<64;i++) if(shared_handles[i] && shared_sources[i]==source && shared_sizes[i]==size && shared_permissions[i]==perm) return i;
 *temporary=1;return copy_shared_object(source,perm,size)-0x4000;
}
static void ss_shared_drop(unsigned slot) {
 CloseHandle(shared_handles[slot]);shared_handles[slot]=NULL;shared_sources[slot]=NULL;shared_permissions[slot]=0;shared_sizes[slot]=0;
}
/* Loading, before memory is copied in: stack mappings, transfer memory, thread-local blocks (and for a state of
 * another process: the shared memory views and handles) as they were when the state was saved. ss_unmap_extra runs
 * on the tables of now; ss_map_saved after the other tables are the saved ones (the runtime's own checks then see
 * the saved state). */
static void ss_unmap_extra(void) {
 unsigned stacks=0,tls=0,transfers=0,views=0;
 for(unsigned i=0;i<64;i++) if(stack_maps[i].size && (ss_mem.foreign || !ss_stack_saved(i))) {
  stack_unmap(stack_maps[i].destination,stack_maps[i].source,stack_maps[i].size);stacks++;
 }
 for(unsigned i=0;i<32;i++) if(transfer_memory[i].size) {  /* (all of them: made again in their saved slots) */
  DWORD previous;
  VirtualProtect((void*)(uintptr_t)transfer_memory[i].address,(SIZE_T)transfer_memory[i].size,PAGE_READWRITE,&previous);
  VirtualFree(transfer_memory[i].data,0,MEM_RELEASE);
  memset(&transfer_memory[i],0,sizeof(transfer_memory[i]));transfers++;
 }
 for(unsigned i=0;i<32;i++) if(thread_tls[i] && thread_tls[i]!=ss_mem.tls[i]) {
  if(!VirtualFree((void*)(uintptr_t)thread_tls[i],0,MEM_RELEASE)) boundary("ss-tls-release",&state,state.pc);
  thread_tls[i]=0;tls++;
 }
 if(ss_mem.foreign) {
  for(unsigned i=0;i<64;i++) if(shared_views[i].address) {
   int temporary;unsigned slot=ss_shared_slot(shared_views[i].source,shared_views[i].permission,shared_views[i].size,&temporary);
   shared_unmap(0x4000+slot,shared_views[i].address,shared_views[i].size);views++;
   if(temporary) ss_shared_drop(slot);
  }
  for(unsigned i=0;i<64;i++) if(shared_handles[i]) ss_shared_drop(i);
 }
 if(stacks || tls || transfers || views)
  printf("[ss] memory map: %u stack mappings, %u thread-local blocks, %u transfer memories, %u shared views removed\n",stacks,tls,transfers,views);
}
static void ss_map_saved(void) {
 unsigned stacks=0,tls=0,transfers=0,views=0;
 if(ss_mem.foreign) {
  for(unsigned i=0;i<64;i++) if(ss_mem.shared[i].kind==1 || ss_mem.shared[i].kind==2) {
   HANDLE source=ss_mem.shared[i].kind==1 ? hid_memory : clock_memory;
   if(!source || !DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&shared_handles[i],0,FALSE,DUPLICATE_SAME_ACCESS))
    boundary("ss-shared-handle",&state,state.pc);
   shared_sources[i]=source;shared_permissions[i]=ss_mem.shared[i].perm;shared_sizes[i]=ss_mem.shared[i].size;
  }
  for(unsigned i=0;i<64;i++) if(ss_mem.views[i].kind==1 || ss_mem.views[i].kind==2) {
   HANDLE source=ss_mem.views[i].kind==1 ? hid_memory : clock_memory;
   int temporary;unsigned slot=ss_shared_slot(source,ss_mem.views[i].perm,ss_mem.views[i].size,&temporary);
   shared_map(0x4000+slot,ss_mem.views[i].address,ss_mem.views[i].size,ss_mem.views[i].perm);views++;
   if(temporary) ss_shared_drop(slot);
  }
 }
 for(unsigned i=0;i<32;i++) if(ss_mem.tls[i] && thread_tls[i]!=ss_mem.tls[i]) {
  if(VirtualAlloc((void*)(uintptr_t)ss_mem.tls[i],65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)!=(void*)(uintptr_t)ss_mem.tls[i]) {
   fprintf(stderr,"[ss] thread-local block at 0x%llx: address taken (error %lu)\n",(unsigned long long)ss_mem.tls[i],(unsigned long)GetLastError());
   boundary("ss-tls-address",&state,state.pc);
  }
  thread_tls[i]=ss_mem.tls[i];tls++;
 }
 for(unsigned j=0;j<64;j++) if(ss_mem.stacks[j].size) {
  unsigned i=0;
  while(i<64 && (ss_mem.stacks[j].size!=stack_maps[i].size || ss_mem.stacks[j].source!=stack_maps[i].source ||
                 ss_mem.stacks[j].destination!=stack_maps[i].destination)) i++;
  if(i==64) {stack_map(ss_mem.stacks[j].destination,ss_mem.stacks[j].source,ss_mem.stacks[j].size);stacks++;}
 }
 for(unsigned i=0;i<32;i++) if(ss_mem.transfers[i].size) {
  uint64_t size=ss_mem.transfers[i].size;DWORD previous;
  void *data=VirtualAlloc(NULL,(SIZE_T)size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
  if(!data) boundary("ss-transfer-memory",&state,state.pc);
  memcpy(data,ss_mem.transfer_data[i],size);
  transfer_memory[i]=ss_mem.transfers[i];transfer_memory[i].data=data;
  if(!(heap_gpu_pinned && transfer_memory[i].address>=heap_base && transfer_memory[i].address-heap_base<HEAP_LIMIT))
   VirtualProtect((void*)(uintptr_t)transfer_memory[i].address,(SIZE_T)size,PAGE_NOACCESS,&previous);
  transfers++;
 }
 if(!ss_mem.foreign) {
  unsigned missing=0;
  for(unsigned i=0;i<64;i++) if(ss_mem.views[i].kind==1 || ss_mem.views[i].kind==2) {
   unsigned j=0;while(j<64 && (shared_views[j].address!=ss_mem.views[i].address || shared_views[j].size!=ss_mem.views[i].size)) j++;
   missing+=j==64;
  }
  if(missing) printf("[ss] %u shared memory views of the state are not mapped now (not handled)\n",missing);
 }
 if(stacks || tls || transfers || views)
  printf("[ss] memory map: %u stack mappings, %u thread-local blocks, %u transfer memories, %u shared views made\n",stacks,tls,transfers,views);
 InterlockedIncrement(&mapping_generation);
}
static void ss_note(long code) {InterlockedExchange(&ac_ss_note,code);ac_ss_note_tick=GetTickCount64();}
static const wchar_t *ss_file_path(void) {
 static wchar_t path[600];
 if(!path[0]) {
  DWORD n=GetEnvironmentVariableW(L"AC_SS_FILE",path,600);
  if(!n || n>=600) {
   /* beside the user's save folder: ...\saves\<title>\<user> -> ...\saves\<title>\savestates */
   wchar_t folder[400];const wchar_t *root=application_save.ready ? application_save.root : application_save_path();
   wcsncpy(folder,root,300);folder[300]=0;
   wchar_t *slash=wcsrchr(folder,L'\\');
   if(slash && slash>folder) {
    wcscpy(slash,L"\\savestates");CreateDirectoryW(folder,NULL);
    swprintf(path,600,L"%ls\\state.acss",folder);
   } else wcscpy(path,L"state.acss");
  }
 }
 return path;
}
static int ss_file_write(const wchar_t *path);
/* The world is stopped (kernel lock held). */
static int ss_save(Cpu *c,uint64_t after) {
 LARGE_INTEGER a,b,f;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&a);
 for(unsigned i=0;i<64;i++) {  /* (an open save file cannot be put into a state) */
  int open=sessions[i].save_file!=NULL;
  for(unsigned j=0;j<32;j++) open|=sessions[i].object_save_file[j]!=NULL;
  if(open) {printf("[ss] not saved: the game has a save file open; try again in a moment\n");fflush(stdout);return 0;}
 }
 nvn_lock();
 nvn_async_drain();
 vk_ss_quiesce();
 if(!ss_mem.runs) ss_mem.runs=malloc(65536*sizeof(SsRun));
 uint64_t bytes=0;
 ss_mem.n=ss_guest_runs(ss_mem.runs,65536,&bytes);
 ss_mem.bytes=bytes;ss_mem.heap_size=heap_size;ss_mem.heap_base=heap_base;ss_mem.generation=mapping_generation;ss_mem.ticks=rt_ticks();
 ss_mem.pid=AC_PID();ss_mem.res_w=res_w;ss_mem.res_h=res_h;ss_mem.foreign=0;
 for(unsigned i=0;i<SS_NSTATIC;i++) {
  if(!ss_statics[i].copy) ss_statics[i].copy=malloc(ss_statics[i].n);
  memcpy(ss_statics[i].copy,ss_statics[i].p,ss_statics[i].n);
 }
 memcpy(ss_mem.tls,thread_tls,sizeof(ss_mem.tls));memcpy(ss_mem.stacks,stack_maps,sizeof(ss_mem.stacks));
 for(unsigned i=0;i<64;i++) {
  ss_mem.shared[i]=(SsShared){0,0,0};ss_mem.views[i]=(SsView){0,0,0,0};
  if(shared_handles[i]) ss_mem.shared[i]=(SsShared){shared_sources[i]==hid_memory ? 1 : shared_sources[i]==clock_memory ? 2 : 3,
   (uint8_t)shared_permissions[i],shared_sizes[i]};
  if(shared_views[i].address) ss_mem.views[i]=(SsView){shared_views[i].address,shared_views[i].size,
   shared_views[i].source==hid_memory ? 1 : shared_views[i].source==clock_memory ? 2 : 3,(uint8_t)shared_views[i].permission};
 }
 for(unsigned i=0;i<32;i++) {
  if(ss_mem.transfer_data[i]) {VirtualFree(ss_mem.transfer_data[i],0,MEM_RELEASE);ss_mem.transfer_data[i]=NULL;}
  ss_mem.transfers[i]=transfer_memory[i];
  if(transfer_memory[i].size) {
   ss_mem.transfer_data[i]=VirtualAlloc(NULL,(SIZE_T)transfer_memory[i].size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
   if(ss_mem.transfer_data[i]) memcpy(ss_mem.transfer_data[i],transfer_memory[i].data,transfer_memory[i].size);
  }
 }
 unsigned objects=0;
 for(unsigned k=0;k<SS_NEV;k++) {
  int external;HANDLE h=*ss_ev_field(k,&external);
  ss_mem.ev_obj[k]=-1;
  if(!h) continue;
  for(unsigned j=0;j<k && ss_mem.ev_obj[k]<0;j++) {int e;HANDLE o=*ss_ev_field(j,&e);if(o && ss_mem.ev_obj[j]>=0 && ss_same_object(h,o)) ss_mem.ev_obj[k]=ss_mem.ev_obj[j];}
  if(ss_mem.ev_obj[k]<0) {ss_mem.ev_sig[objects]=WaitForSingleObject(h,0)==WAIT_OBJECT_0;ss_mem.ev_obj[k]=(int16_t)objects++;}
 }
 for(unsigned i=0;i<16;i++) {
  free(ss_mem.applet[i].data);ss_mem.applet[i].data=NULL;
  ss_mem.applet[i].size=applet_storage[i].size;ss_mem.applet[i].refs=applet_storage[i].refs;ss_mem.applet[i].has=applet_storage[i].data!=NULL;
  if(applet_storage[i].data) {ss_mem.applet[i].data=malloc(applet_storage[i].size ? applet_storage[i].size : 1);memcpy(ss_mem.applet[i].data,applet_storage[i].data,applet_storage[i].size);}
 }
 for(unsigned i=0;i<16;i++) {
  WaveOutput *wave=&audio_outputs[i].wave;
  memset(&ss_mem.audio[i],0,sizeof(ss_mem.audio[i]));
  ss_mem.audio[i].active=audio_outputs[i].active;ss_mem.audio[i].running=wave->running;
  ss_mem.audio[i].rate=wave->format.nSamplesPerSec;ss_mem.audio[i].channels=wave->stream_channels;
  for(unsigned k=0;k<audio_outputs[i].ndropped && ss_mem.audio[i].n<64;k++) ss_mem.audio[i].tags[ss_mem.audio[i].n++]=audio_outputs[i].dropped[k];
  for(unsigned k=0;k<wave->count && ss_mem.audio[i].n<64;k++) ss_mem.audio[i].tags[ss_mem.audio[i].n++]=wave->buffers[(wave->head+k)%32].tag;
 }
 if(!ss_mem.hid) ss_mem.hid=malloc(HID_MEMORY_SIZE);
 if(!ss_mem.clock) ss_mem.clock=malloc(4096);
 if(hid_page) memcpy(ss_mem.hid,hid_page,HID_MEMORY_SIZE); else memset(ss_mem.hid,0,HID_MEMORY_SIZE);
 if(clock_page) memcpy(ss_mem.clock,clock_page,4096); else memset(ss_mem.clock,0,4096);
 free(ss_mem.thunks);ss_mem.thunk_count=nvn_thunk_count;ss_mem.thunks=malloc((size_t)nvn_thunk_count*NVN_HLE_NAME+1);
 for(unsigned i=0;i<nvn_thunk_count;i++) memcpy(ss_mem.thunks+(size_t)i*NVN_HLE_NAME,nvn_thunks[i].name,NVN_HLE_NAME);
 SsArena *arena=ss_arena();
 if(!ss_mem.arena) ss_mem.arena=VirtualAlloc(NULL,SS_ARENA_SIZE,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
 ss_mem.arena_top=arena->top;memcpy(ss_mem.arena,arena,arena->top);
 void *st=vk_ss_bound_state(&ss_mem.bound_size);
 if(!ss_mem.bound) ss_mem.bound=malloc(ss_mem.bound_size);
 memcpy(ss_mem.bound,st,ss_mem.bound_size);
 ss_mem.nvn_size=nvn_ss_statics(ss_mem.nvn,0);
 ss_capture_threads(c,after);
 for(unsigned i=0;i<33;i++) if(ss_saved.threads[i].status==THREAD_WAIT_GPU) {  /* (the GPU has finished everything) */
  ss_saved.threads[i].status=THREAD_RUNNABLE;ss_saved.threads[i].gpu_done=NULL;ss_saved.threads[i].gpu_seq=0;
 }
 nvn_unlock();
 unsigned threads=0,channels=0;
 for(unsigned i=0;i<33;i++) threads+=ss_saved.phase[i]!=0;
 for(unsigned i=0;i<64;i++) channels+=nv_devices[i].channel.entries!=NULL;
 QueryPerformanceCounter(&b);
 printf("[ss] saved at step %llu: %.0f MB of guest memory in %u runs, %u threads, %llu NVN objects (%.1f MB), %u event objects, %.0f ms\n",
  (unsigned long long)ac_game_steps,(double)bytes/1048576.0,ss_mem.n,threads,(unsigned long long)arena->live,
  (double)arena->top/1048576.0,objects,1000.0*(double)(b.QuadPart-a.QuadPart)/(double)f.QuadPart);
 if(channels) printf("[ss] %u GPU channels are open in the driver emulation (not put into the state)\n",channels);
 fflush(stdout);
 return ss_file_write(ss_file_path());  /* (the memory itself goes from guest memory into the file: the world is stopped) */
}
/* ---- the file ---- */
typedef struct { char magic[8],build[32]; uint32_t pid,nruns,thunk_count,thunk_name,nstatic; int32_t res_w,res_h;
 uint64_t ticks,steps,heap_base,heap_size,bytes,arena_top,bound_size,nvn_size,threads_size,session,file_size; } SsHeader;
static uint64_t ss_session(void) {  /* names this run of the game */
 static uint64_t id;
 if(!id) {LARGE_INTEGER t;QueryPerformanceCounter(&t);id=((uint64_t)t.QuadPart*0x9E3779B97F4A7C15ull)^((uint64_t)GetCurrentProcessId()<<32)^1;}
 return id;
}
static int ss_io(FILE *file,void *p,uint64_t n,int write) {
 uint8_t *at=p;
 while(n) {
  size_t part=n>(64u<<20) ? (64u<<20) : (size_t)n;
  if((write ? fwrite(at,1,part,file) : fread(at,1,part,file))!=part) return 0;
  at+=part;n-=part;
 }
 return 1;
}
static int ss_file_sections(FILE *file,int write) {
#define IO(p,n) do {if(!ss_io(file,(p),(n),write)) return 0;} while(0)
 IO(&ss_saved,sizeof(ss_saved));
 for(unsigned i=0;i<SS_NSTATIC;i++) {
  char name[32]={0};uint64_t size=ss_statics[i].n;
  strncpy(name,ss_statics[i].name,31);
  char had[32];memcpy(had,name,32);
  IO(name,32);IO(&size,8);
  if(memcmp(had,name,32) || size!=ss_statics[i].n) return 0;
  if(!ss_statics[i].copy) ss_statics[i].copy=malloc(ss_statics[i].n);
  IO(ss_statics[i].copy,ss_statics[i].n);
 }
 IO(ss_mem.tls,sizeof(ss_mem.tls));IO(ss_mem.stacks,sizeof(ss_mem.stacks));IO(ss_mem.transfers,sizeof(ss_mem.transfers));
 for(unsigned i=0;i<32;i++) if(ss_mem.transfers[i].size) {
  if(!write) {
   if(ss_mem.transfer_data[i]) VirtualFree(ss_mem.transfer_data[i],0,MEM_RELEASE);
   ss_mem.transfer_data[i]=VirtualAlloc(NULL,(SIZE_T)ss_mem.transfers[i].size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
  }
  if(!ss_mem.transfer_data[i]) return 0;
  IO(ss_mem.transfer_data[i],ss_mem.transfers[i].size);
 }
 IO(ss_mem.shared,sizeof(ss_mem.shared));IO(ss_mem.views,sizeof(ss_mem.views));IO(ss_mem.audio,sizeof(ss_mem.audio));
 IO(ss_mem.ev_obj,sizeof(ss_mem.ev_obj));IO(ss_mem.ev_sig,sizeof(ss_mem.ev_sig));
 for(unsigned i=0;i<16;i++) {
  IO(&ss_mem.applet[i].size,8);IO(&ss_mem.applet[i].refs,4);IO(&ss_mem.applet[i].has,4);
  if(!ss_mem.applet[i].has) continue;
  if(!write) {free(ss_mem.applet[i].data);ss_mem.applet[i].data=malloc(ss_mem.applet[i].size ? ss_mem.applet[i].size : 1);}
  IO(ss_mem.applet[i].data,ss_mem.applet[i].size);
 }
 if(!ss_mem.hid) ss_mem.hid=malloc(HID_MEMORY_SIZE);
 if(!ss_mem.clock) ss_mem.clock=malloc(4096);
 IO(ss_mem.hid,HID_MEMORY_SIZE);IO(ss_mem.clock,4096);
 if(!ss_mem.arena) ss_mem.arena=VirtualAlloc(NULL,SS_ARENA_SIZE,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
 IO(ss_mem.arena,ss_mem.arena_top);
 if(!ss_mem.bound) ss_mem.bound=malloc(ss_mem.bound_size);
 IO(ss_mem.bound,ss_mem.bound_size);IO(ss_mem.nvn,ss_mem.nvn_size);
 if(!write) {free(ss_mem.thunks);ss_mem.thunks=malloc((size_t)ss_mem.thunk_count*NVN_HLE_NAME+1);}
 IO(ss_mem.thunks,(uint64_t)ss_mem.thunk_count*NVN_HLE_NAME);
#undef IO
 return 1;
}
static int ss_page_zero(const uint8_t *p) {
 const uint64_t *q=(const uint64_t*)p;
 for(unsigned i=0;i<512;i++) if(q[i]) return 0;
 return 1;
}
static int ss_file_write(const wchar_t *path) {
 LARGE_INTEGER a,b,f;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&a);
 wchar_t temporary[700];swprintf(temporary,700,L"%ls.new",path);
 FILE *file=_wfopen(temporary,L"wb");
 if(!file) {printf("[ss] cannot write %ls\n",temporary);fflush(stdout);return 0;}
 setvbuf(file,NULL,_IOFBF,4u<<20);
 SsHeader h={0};
 memcpy(h.magic,"ACSS0002",8);strncpy(h.build,ss_build(),31);
 h.pid=ss_mem.pid;h.nruns=ss_mem.n;h.thunk_count=ss_mem.thunk_count;h.thunk_name=NVN_HLE_NAME;h.nstatic=SS_NSTATIC;
 h.res_w=ss_mem.res_w;h.res_h=ss_mem.res_h;h.ticks=ss_mem.ticks;h.steps=ss_saved.steps;h.heap_base=ss_mem.heap_base;
 h.heap_size=ss_mem.heap_size;h.bytes=ss_mem.bytes;h.arena_top=ss_mem.arena_top;h.bound_size=ss_mem.bound_size;
 h.nvn_size=ss_mem.nvn_size;h.threads_size=sizeof(ss_saved);h.session=ss_session();
 int ok=ss_io(file,&h,sizeof(h),1) && ss_io(file,ss_mem.runs,(uint64_t)ss_mem.n*sizeof(SsRun),1) && ss_file_sections(file,1);
 uint64_t kept=0;
 for(unsigned i=0;ok && i<ss_mem.n;i++) {  /* per run: one bit per page (not all zero), then those pages */
  uint64_t pages=ss_mem.runs[i].size>>12;const uint8_t *base=(const uint8_t*)(uintptr_t)ss_mem.runs[i].addr;
  uint8_t *bits=calloc(1,(pages+7)/8+1);
  for(uint64_t p=0;p<pages;p++) if(!ss_page_zero(base+(p<<12))) bits[p>>3]|=(uint8_t)(1u<<(p&7));
  ok=ss_io(file,bits,(pages+7)/8,1);
  for(uint64_t p=0;ok && p<pages;) {
   if(!(bits[p>>3]&(1u<<(p&7)))) {p++;continue;}
   uint64_t q=p;while(q<pages && (bits[q>>3]&(1u<<(q&7)))) q++;
   ok=ss_io(file,(void*)(base+(p<<12)),(q-p)<<12,1);kept+=(q-p)<<12;p=q;
  }
  free(bits);
 }
 if(ok) {  /* (the whole size, for the reader: a file cut short is refused before anything is changed) */
  h.file_size=(uint64_t)_ftelli64(file);
  ok=!_fseeki64(file,0,SEEK_SET) && ss_io(file,&h,sizeof(h),1);
 }
 ok=fclose(file)==0 && ok;
 int written=ok;DWORD error=0;
 /* (another program, a virus scanner for one, can hold the new file open for a moment: tried again for up to 10 s) */
 unsigned tries=0;
 while(ok && !MoveFileExW(temporary,path,MOVEFILE_REPLACE_EXISTING)) {
  error=GetLastError();
  if((error!=ERROR_SHARING_VIOLATION && error!=ERROR_ACCESS_DENIED) || ++tries>=100) {ok=0;break;}
  Sleep(100);
 }
 if(ok && tries) printf("[ss] the state file was held open by another program for %.1f s\n",tries*0.1);
 if(!ok) _wremove(temporary);
 QueryPerformanceCounter(&b);
 wchar_t full[700];if(!GetFullPathNameW(path,700,full,NULL)) wcscpy(full,path);
 if(ok) printf("[ss] state file %ls: %.0f MB (%.0f MB of the memory is zero pages, left out), %.0f ms\n",full,
  (double)h.file_size/1048576.0,(double)(ss_mem.bytes-kept)/1048576.0,1000.0*(double)(b.QuadPart-a.QuadPart)/(double)f.QuadPart);
 else printf("[ss] writing %ls failed (%s, Windows error %lu)\n",full,written ? "renaming the finished file" : "writing",(unsigned long)error);
 fflush(stdout);
 return ok;
}
/* Opens a state file and reads all of it but the memory pages; ss_mem.file is left open at them. 0: not usable (said
 * in the log), nothing was changed in the game. */
static int ss_file_read(const wchar_t *path) {
 FILE *file=_wfopen(path,L"rb");
 if(!file) {printf("[ss] no state file %ls\n",path);fflush(stdout);return 0;}
 setvbuf(file,NULL,_IOFBF,4u<<20);
 SsHeader h;const char *why=NULL;
 static int any=-1;
 if(any<0) {const char *e=getenv("AC_SS_ANY_BUILD");any=e && atoi(e);}
 if(!ss_io(file,&h,sizeof(h),0) || memcmp(h.magic,"ACSS",4)) why="not a state file";
 else if(memcmp(h.magic,"ACSS0002",8) || (strncmp(h.build,ss_build(),31) && !any)) why="made by another version of the port or of the game";
 else if(h.thunk_name!=NVN_HLE_NAME || h.nstatic!=SS_NSTATIC || h.threads_size!=sizeof(ss_saved) || h.nruns>65536 ||
         h.arena_top>SS_ARENA_SIZE || h.nvn_size>sizeof(ss_mem.nvn)) why="another layout";
 else if(h.file_size!=(uint64_t)_filelengthi64(_fileno(file))) why="cut short or damaged";
 else if(h.res_w!=res_w || h.res_h!=res_h) why="made at another render resolution";
 else if(h.heap_base!=heap_base) why="the heap is at another address in this run (start the game again)";
 if(why) {
  printf("[ss] %ls: %s\n",path,why);
  if(why[0]=='m' && why[8]=='a' && h.res_w!=res_w) printf("[ss] the state's: %dx%d, this run's: %dx%d\n",h.res_w,h.res_h,res_w,res_h);
  fflush(stdout);fclose(file);return 0;
 }
 ss_saved.valid=0;
 if(!ss_mem.runs) ss_mem.runs=malloc(65536*sizeof(SsRun));
 ss_mem.n=h.nruns;ss_mem.bytes=h.bytes;ss_mem.pid=h.pid;ss_mem.thunk_count=h.thunk_count;ss_mem.res_w=h.res_w;ss_mem.res_h=h.res_h;
 ss_mem.ticks=h.ticks;ss_mem.heap_base=h.heap_base;ss_mem.heap_size=h.heap_size;ss_mem.arena_top=h.arena_top;
 ss_mem.bound_size=h.bound_size;ss_mem.nvn_size=h.nvn_size;ss_mem.same_session=h.session==ss_session();
 if(!ss_io(file,ss_mem.runs,(uint64_t)h.nruns*sizeof(SsRun),0) || !ss_file_sections(file,0)) {
  printf("[ss] %ls: cut short or damaged\n",path);fflush(stdout);fclose(file);return 0;
 }
 ss_mem.foreign=1;ss_saved.valid=1;ss_mem.file=file;
 printf("[ss] %ls: the state of step %llu, %.0f MB of memory\n",path,(unsigned long long)h.steps,(double)h.bytes/1048576.0);
 fflush(stdout);
 return 1;
}
/* The memory pages of ss_mem.file into guest memory (the memory map is the state's by now). Pages the state has as
 * zero are cleared only where they are not zero already. */
static void ss_file_pages(void) {
 FILE *file=ss_mem.file;uint64_t skipped=0;int ok=1;
 uint8_t *scratch=malloc(1u<<20);
 uint64_t rlo=0,rhi=0;int writable=0;
 for(unsigned i=0;ok && i<ss_mem.n;i++) {
  uint64_t pages=ss_mem.runs[i].size>>12,addr=ss_mem.runs[i].addr;
  uint8_t *bits=malloc((pages+7)/8+1);
  ok=ss_io(file,bits,(pages+7)/8,0);
  for(uint64_t p=0;ok && p<pages;) {
   int set=(bits[p>>3]>>(p&7))&1;
   uint64_t q=p+1;while(q<pages && (((bits[q>>3]>>(q&7))&1)==set)) q++;
   for(uint64_t at=addr+(p<<12),end=addr+(q<<12);ok && at<end;) {
    if(at<rlo || at>=rhi) {
     MEMORY_BASIC_INFORMATION m;
     if(!VirtualQuery((void*)(uintptr_t)at,&m,sizeof(m))) {rlo=at;rhi=at+4096;writable=0;}
     else {rlo=(uint64_t)(uintptr_t)m.BaseAddress;rhi=rlo+m.RegionSize;writable=m.State==MEM_COMMIT && m.Protect==PAGE_READWRITE;}
    }
    uint64_t hi=end<rhi ? end : rhi;
    if(set && writable) ok=ss_io(file,(void*)(uintptr_t)at,hi-at,0);
    else if(set) {for(uint64_t left=hi-at;ok && left;) {uint64_t part=left>(1u<<20) ? (1u<<20) : left;ok=ss_io(file,scratch,part,0);left-=part;} skipped+=hi-at;}
    else if(writable) {for(uint64_t z=at;z<hi;z+=4096) if(!ss_page_zero((const uint8_t*)(uintptr_t)z)) memset((void*)(uintptr_t)z,0,4096);}
    else skipped+=hi-at;
    at=hi;
   }
   p=q;
  }
  free(bits);
 }
 free(scratch);fclose(file);ss_mem.file=NULL;
 if(!ok) {  /* (the size was checked before: a read error; the game's memory is half the state's by now) */
  fprintf(stderr,"[ss] reading the state file failed part way; the game cannot go on\n");fflush(stderr);
  boundary("ss-file-read",&state,state.pc);
 }
 if(skipped) printf("[ss] %.1f MB of the state not written back (pages not writable now)\n",(double)skipped/1048576.0);
}
/* The world is stopped (kernel lock held). Does not return: every guest thread continues from the saved state. */
static void ss_load(void) {
 LARGE_INTEGER a,b,f;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&a);
 static int programs=-1;
 if(programs<0) {const char *e=getenv("AC_SS_PROGRAMS");programs=e ? atoi(e) : 0;}
 int foreign=ss_mem.foreign;
 uint64_t steps_now=ac_game_steps,ticks_now=rt_ticks();
 nvn_lock();
 nvn_async_drain();
 vk_ss_drop(programs || !ss_mem.same_session);  /* (another run's programs may sit at the same addresses) */
 ss_unmap_extra();
 for(unsigned i=0;i<(foreign ? SS_NSTATIC : SS_NSTATIC_LOCAL);i++) memcpy(ss_statics[i].p,ss_statics[i].copy,ss_statics[i].n);
 if(foreign) {
  /* what another process's tables name by handle or by address in the program: made again here */
  for(unsigned i=0;i<64;i++) {sessions[i].save_file=NULL;for(unsigned j=0;j<32;j++) sessions[i].object_save_file[j]=NULL;}
  for(unsigned i=0;i<64;i++) {nv_devices[i].channel.entries=NULL;nv_devices[i].channel.zcull_space=NULL;}
  ss_pid=ss_mem.pid;
  if(ss_mem.ticks>ticks_now-ss_tick_offset) ss_tick_offset=ss_mem.ticks-(ticks_now-ss_tick_offset);  /* (the game's clock never runs back) */
  if(hid_page) memcpy(hid_page,ss_mem.hid,HID_MEMORY_SIZE);
  if(clock_page) memcpy(clock_page,ss_mem.clock,4096);
  nvn_thunk_count=0;memset(nvn_thunks,0,sizeof(nvn_thunks));
  for(unsigned i=0;i<ss_mem.thunk_count;i++) nvn_register(ss_mem.thunks+(size_t)i*NVN_HLE_NAME);
 }
 /* Sound: what the device has queued now is not the state's. It is dropped, the device opened again, and every
  * buffer the state's game still waits for is reported as played, so the game queues new sound at once. */
 for(unsigned i=0;i<16;i++) if(ss_mem.audio[i].active) {
  WaveOutput *wave=&audio_outputs[i].wave;uint64_t gone[32];
  unsigned rate=audio_outputs[i].active && wave->format.nSamplesPerSec ? wave->format.nSamplesPerSec : ss_mem.audio[i].rate;
  unsigned channels=audio_outputs[i].active && wave->stream_channels ? wave->stream_channels : ss_mem.audio[i].channels;
  wave_output_abandon(wave,gone,32);
  MMRESULT opened=rate && channels ? wave_output_open(wave,rate,channels) : 1;
  if(!opened && ss_mem.audio[i].running) opened=wave_output_start(wave);
  if(opened) printf("[ss] sound output %u: not opened again (%u)\n",i,(unsigned)opened);
  audio_outputs[i].active=1;audio_outputs[i].ndropped=0;
  for(unsigned k=0;k<ss_mem.audio[i].n && audio_outputs[i].ndropped<32;k++) audio_outputs[i].dropped[audio_outputs[i].ndropped++]=ss_mem.audio[i].tags[k];
  if(wave->completion) SetEvent(wave->completion);
 }
 for(unsigned i=0;i<16;i++) if(vi_layers[i].id) {
  vi_layers[i].producer.retain=vi_retain_buffer;vi_layers[i].producer.release=vi_release_buffer;
  vi_layers[i].producer.consume=vi_consume_buffer;vi_layers[i].producer.context=(void*)(uintptr_t)vi_layers[i].id;
 }
 {HANDLE source[SS_NEV]={0};uint8_t own[SS_NEV]={0};
  for(unsigned k=0;k<SS_NEV;k++) {
   int external;HANDLE *field=ss_ev_field(k,&external);int o=ss_mem.ev_obj[k];
   if(external && o>=0 && *field) source[o]=*field;
  }
  for(unsigned k=0;k<SS_NEV;k++) {
   int external;HANDLE *field=ss_ev_field(k,&external);int o=ss_mem.ev_obj[k];
   if(external) continue;
   if(!foreign) {if(o>=0 && *field) {if(ss_mem.ev_sig[o]) SetEvent(*field); else ResetEvent(*field);} continue;}
   if(o<0) {*field=NULL;continue;}
   if(!source[o]) {source[o]=CreateEventW(NULL,TRUE,ss_mem.ev_sig[o],NULL);own[o]=1;}
   if(!source[o] || !DuplicateHandle(GetCurrentProcess(),source[o],GetCurrentProcess(),field,0,FALSE,DUPLICATE_SAME_ACCESS))
    boundary("ss-event",&state,state.pc);
  }
  for(unsigned o=0;o<SS_NEV;o++) if(own[o]) CloseHandle(source[o]);
 }
 for(unsigned i=0;i<16;i++) {
  free(applet_storage[i].data);applet_storage[i].data=NULL;
  applet_storage[i].size=ss_mem.applet[i].size;applet_storage[i].refs=ss_mem.applet[i].refs;
  if(ss_mem.applet[i].has) {
   applet_storage[i].data=calloc(1,ss_mem.applet[i].size ? ss_mem.applet[i].size : 1);
   memcpy(applet_storage[i].data,ss_mem.applet[i].data,ss_mem.applet[i].size);
  }
 }
 if(heap_size!=ss_mem.heap_size) {
  printf("[ss] heap size 0x%llx, saved at 0x%llx\n",(unsigned long long)heap_size,(unsigned long long)ss_mem.heap_size);
  if(resize_heap(ss_mem.heap_size)) boundary("ss-heap-resize",&state,state.pc);
 }
 ss_map_saved();
 ss_file_pages();
 InterlockedIncrement(&mapping_generation);
 memcpy(ss_arena(),ss_mem.arena,ss_mem.arena_top);
 nvn_ss_reset_backends();
 size_t bound;void *st=vk_ss_bound_state(&bound);
 if(bound==ss_mem.bound_size) memcpy(st,ss_mem.bound,bound);
 vk_ss_fix_bound();
 nvn_ss_statics(ss_mem.nvn,1);
 for(unsigned i=0;i<33;i++) guest_threads[i]=ss_saved.threads[i];
 QueryPerformanceCounter(&b);
 printf("[ss] loaded the state of step %llu at step %llu%s, %.0f ms\n",(unsigned long long)ss_saved.steps,
  (unsigned long long)steps_now,ss_mem.same_session ? "" : " (of another run)",1000.0*(double)(b.QuadPart-a.QuadPart)/(double)f.QuadPart);
 fflush(stdout);
 ss_note(2);
 ss_restart_threads();
}
/* svc #0xB1 on the game thread, after the call's own work (kernel lock held once). `after`: the next instruction.
 * With AC_SAVE_STATES=1: F6 writes the state file, F7 loads it. Tests: AC_SS_CYCLE=n: every n steps stop the threads
 * and restart them as they are; AC_SS_SAVE_AT=step, AC_SS_LOAD_AT=step[,step...]: save or load at those game steps. */
static void ss_frame(Cpu *c,uint64_t after) {
 static int cycle=-1;static uint64_t save_at,load_at[64];static unsigned loads,next_load;
 if(!parallel_mode || !ss_enabled()) return;
 if(cycle<0) {
  const char *e=getenv("AC_SS_CYCLE");cycle=e ? atoi(e) : 0;
  e=getenv("AC_SS_SAVE_AT");save_at=e ? strtoull(e,NULL,10) : 0;
  e=getenv("AC_SS_LOAD_AT");
  while(e && *e && loads<64) {char *end;load_at[loads++]=strtoull(e,&end,10);e=*end==',' ? end+1 : NULL;}
 }
 if(cycle>0 && ac_game_steps%(uint64_t)cycle==0) {  /* (stop, capture the threads, restart them as they are) */
  if(!ss_stop_world()) return;
  ss_capture_threads(c,after);
  ss_restart_threads();
 }
 if((save_at && ac_game_steps>=save_at) || ss_key==1) {
  if(!ss_stop_world()) return;  /* (tried again at the next step) */
  if(ss_key==1) InterlockedExchange(&ss_key,0); else save_at=0;
  ss_note(ss_save(c,after) ? 1 : 3);
  InterlockedExchange(&ss_stop,0);
  return;
 }
 if((next_load<loads && ac_game_steps>=load_at[next_load]) || ss_key==2) {
  if(ss_key==2) InterlockedExchange(&ss_key,0); else next_load++;
  if(!ss_file_read(ss_file_path())) {ss_note(_waccess(ss_file_path(),0) ? 4 : 5);return;}
  if(!ss_stop_world()) {
   fclose(ss_mem.file);ss_mem.file=NULL;ss_note(5);
   printf("[ss] not loaded: the game's threads did not stop; try again\n");fflush(stdout);return;
  }
  ss_load();
 }
}
