/* Serialized guest contexts. This is a bounded bring-up scheduler, not a
 * timing-accurate model of Horizon's multicore scheduler. Each virtual core
 * receives a turn; its highest-priority runnable thread is selected, with
 * round-robin selection among equal priorities. Switch only at instruction
 * boundaries, after the lifter has advanced the calling thread's PC. */
#ifndef MAIN_CORE
#define MAIN_CORE 0
#endif
#ifndef PROCESS_CORE_MASK
#define PROCESS_CORE_MASK 15ull
#endif
#ifndef PROCESS_PRIORITY_MASK
#define PROCESS_PRIORITY_MASK (~0ull)
#endif
enum { THREAD_UNUSED, THREAD_CREATED, THREAD_RUNNABLE, THREAD_EXITED, THREAD_WAIT_MUTEX, THREAD_SLEEPING, THREAD_WAIT_CONDITION, THREAD_WAIT_ADDRESS, THREAD_WAIT_OBJECTS, THREAD_WAIT_GPU };
static struct { Cpu cpu; unsigned status,priority,core,handle_open; int ideal_core;
 uint64_t id,affinity,mutex,wake_tick,condition_key,condition_deadline,wait_order,address_key,address_deadline;
 unsigned mutex_tag,mutex_owner;
 /* WaitSynchronization: thread (0x1000/0x5000+) or event (0x3000+) handles. */
 unsigned wait_count;uint32_t wait_handles[64];uint64_t wait_deadline;
 /* THREAD_WAIT_GPU: runnable once gpu_done(gpu_seq) (NVN fence waits). */
 uint64_t gpu_seq;int (*gpu_done)(uint64_t); } guest_threads[33];
static __thread unsigned current_thread;  /* (per host thread: in parallel mode each runs one guest thread) */
/* The live context of guest thread i. Serialized: the running thread's is `state`, the others' are saved here.
 * Parallel: each host thread's own state (live_cpu[i]). */
static Cpu *thread_cpu(unsigned i) {
 if(parallel_mode) return live_cpu[i];
 return i==current_thread ? &state : &guest_threads[i].cpu;
}
static uint64_t next_thread_id=0x100000000ull;
static uint64_t next_wait_order;
unsigned thread_reschedule;
static void threads_initialize(void) {
 if(guest_threads[0].status) return;
 guest_threads[0].status=THREAD_RUNNABLE;guest_threads[0].priority=MAIN_PRIORITY;
 guest_threads[0].core=MAIN_CORE;guest_threads[0].handle_open=1;
 guest_threads[0].ideal_core=MAIN_CORE;guest_threads[0].affinity=1ull<<MAIN_CORE;
 guest_threads[0].id=GetCurrentThreadId();
}
static unsigned thread_index(uint64_t handle) {
 threads_initialize();
 if(handle==0xffff8000) return current_thread;
 if(handle==0x1000 && guest_threads[0].handle_open) return 0;
 if(handle>=0x5000 && handle<0x5020 && guest_threads[handle-0x5000+1].handle_open)
  return (unsigned)(handle-0x5000+1);
 boundary("invalid-thread-handle",&state,state.pc);
}
/* Private bring-up allocation policy: first-fit 64-KiB TLS blocks below the
 * preferred heap, within the guest's 39-bit VA. Both guest mappings and host
 * reservations must leave the entire block free. No guest address is fixed. */
static uint64_t thread_allocate_tls(void) {
 const uint64_t end=0x7200000000ull,alignment=65536;
 for(uint64_t tls=0x7102000000ull;tls<=end-alignment;) {
  Region space=query(tls);uint64_t next=tls+alignment;
  if(space.kind || space.size-(tls-space.base)<alignment) {
   next=space.base+space.size;
  } else {
   MEMORY_BASIC_INFORMATION host;
   if(!VirtualQuery((void*)(uintptr_t)tls,&host,sizeof(host))) return 0;
   uint64_t host_end=(uint64_t)(uintptr_t)host.BaseAddress+host.RegionSize;
   if(host.State!=MEM_FREE || host_end-tls<alignment) next=host_end;
   else {
    void *allocation=VirtualAlloc((void*)(uintptr_t)tls,alignment,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(allocation==(void*)(uintptr_t)tls) return tls;
    DWORD error=GetLastError();
    if(allocation) VirtualFree(allocation,0,MEM_RELEASE);
    /* A racing host reservation may take this candidate. Other allocation
     * failures are resource errors, not reasons to scan the whole arena. */
    if(error!=ERROR_INVALID_ADDRESS) return 0;
   }
  }
  if(next>=end) break;
  next=(next+alignment-1)&~(alignment-1);
  if(next<=tls) return 0;
  tls=next;
 }
 return 0;
}
static uint32_t thread_create(Cpu *c) {
 uint64_t entry=c->x[1],stack=c->x[3];
 unsigned priority=(uint32_t)c->x[4];int core=(int32_t)c->x[5];
 if(core==-2) core=MAIN_CORE;
 if(priority>63 || core<0 || core>3 || !(PROCESS_CORE_MASK&(1ull<<core)) ||
    !(PROCESS_PRIORITY_MASK&(1ull<<priority))) boundary("thread-priority-or-core",c,c->pc);
 Region code=query(entry),memory=query(stack ? stack-1 : 0);
 if((entry&3) || !(code.permission&4) || code.size-(entry-code.base)<4)
  boundary("thread-entry-memory",c,c->pc);
 if(!stack || (stack&15) || memory.permission!=3 || stack-memory.base<16)
  boundary("thread-stack-memory",c,c->pc);
 threads_initialize();
 unsigned slot;
 for(slot=1;slot<33 && guest_threads[slot].status!=THREAD_UNUSED;slot++);
 if(slot==33) boundary("thread-capacity",c,c->pc);
 uint64_t tls=thread_allocate_tls();if(!tls) return 0xd001;
 thread_tls[slot-1]=tls;
 memset(&guest_threads[slot],0,sizeof(guest_threads[slot]));
 Cpu *child=&guest_threads[slot].cpu;
 child->pc=entry;child->sp=stack;child->x[0]=c->x[2];child->tpidrro=tls;child->excl_addr=~0ull;
 guest_threads[slot].status=THREAD_CREATED;guest_threads[slot].priority=priority;
 guest_threads[slot].core=core;guest_threads[slot].handle_open=1;guest_threads[slot].id=next_thread_id++;
 guest_threads[slot].ideal_core=core;guest_threads[slot].affinity=1ull<<core;
 c->x[1]=0x5000+slot-1;
 printf("THREAD created handle=0x%llx entry=0x%llx tls=0x%llx priority=%u core=%d\n",
  (unsigned long long)c->x[1],(unsigned long long)entry,(unsigned long long)tls,priority,core);
 return 0;
}
static void thread_priorities(unsigned effective[33]) {
 for(unsigned i=0;i<33;i++) effective[i]=guest_threads[i].priority;
 /* Bounded transitive priority donation along mutex ownership edges. */
 for(unsigned pass=0;pass<33;pass++) {
  int changed=0;
  for(unsigned i=0;i<33;i++) if(guest_threads[i].status==THREAD_WAIT_MUTEX) {
   unsigned owner=guest_threads[i].mutex_owner;
   if(effective[i]<effective[owner]) {effective[owner]=effective[i];changed=1;}
  }
  if(!changed) break;
 }
}
static void thread_mutex_memory(uint64_t address) {
 Region r=query(address);
 if((address&3) || r.permission!=3 || r.size-(address-r.base)<4)
  boundary("thread-mutex-memory",&state,state.pc);
}
static void thread_mutex_unlock(uint64_t address) {
 thread_mutex_memory(address);
 unsigned chosen=33,best=64,count=0,effective[33];thread_priorities(effective);
 for(unsigned i=0;i<33;i++) if(guest_threads[i].status==THREAD_WAIT_MUTEX && guest_threads[i].mutex==address && guest_threads[i].mutex_owner==current_thread) {
  count++;
  if(effective[i]<best || (effective[i]==best && chosen<33 && guest_threads[i].wait_order<guest_threads[chosen].wait_order)) {chosen=i;best=effective[i];}
 }
 uint32_t tag=0;
 if(chosen<33) {
  tag=guest_threads[chosen].mutex_tag | (count>1 ? 0x40000000u : 0);
  guest_threads[chosen].status=THREAD_RUNNABLE;guest_threads[chosen].mutex=0;parallel_wake(chosen);
  guest_threads[chosen].condition_deadline=0;
  for(unsigned i=0;i<33;i++) if(guest_threads[i].status==THREAD_WAIT_MUTEX && guest_threads[i].mutex==address && guest_threads[i].mutex_owner==current_thread)
   guest_threads[i].mutex_owner=chosen;
 }
 ST32(address,tag);(parallel_mode ? 0 : (thread_reschedule=1));
}
static uint64_t thread_deadline(int64_t ns) {
 return ns>0 ? rt_ticks()+(uint64_t)(((__uint128_t)(uint64_t)ns*19200000+999999999)/1000000000) : 0;
}
static void thread_condition_signal(uint64_t key,int32_t count) {
 thread_mutex_memory(key);
 unsigned signaled=0;
 for(;;) {
  unsigned chosen=33,effective[33];thread_priorities(effective);
  for(unsigned i=0;i<33;i++) if(guest_threads[i].status==THREAD_WAIT_CONDITION && guest_threads[i].condition_key==key) {
   if(chosen==33 || effective[i]<effective[chosen] ||
      (effective[i]==effective[chosen] && guest_threads[i].wait_order<guest_threads[chosen].wait_order)) chosen=i;
  }
  if(chosen==33) {ST32(key,0);break;}
  if(count>0 && signaled>=(unsigned)count) break;
  uint64_t address=guest_threads[chosen].mutex;thread_mutex_memory(address);
  uint32_t owner=LD32(address);
  guest_threads[chosen].condition_key=0;
  if(!owner) {
   ST32(address,guest_threads[chosen].mutex_tag);
   guest_threads[chosen].status=THREAD_RUNNABLE;guest_threads[chosen].mutex=0;parallel_wake(chosen);
   guest_threads[chosen].condition_deadline=0;
  } else {
   unsigned index=thread_index(owner&~0x40000000u);
   if(index==chosen || guest_threads[index].status==THREAD_EXITED) boundary("condition-mutex-owner",&state,state.pc);
   ST32(address,owner|0x40000000u);
   guest_threads[chosen].status=THREAD_WAIT_MUTEX;guest_threads[chosen].mutex_owner=index;
   guest_threads[chosen].wait_order=++next_wait_order;
   /* The original timeout remains armed while reacquiring the mutex. */
  }
  signaled++;(parallel_mode ? 0 : (thread_reschedule=1));
 }
}
/* 32-bit address arbitration is atomic with respect to guest execution: the
 * scheduler cannot interleave another context inside either syscall. Waiters
 * are separate from condition variables and do not acquire a mutex. */
static uint32_t thread_address_svc(Cpu *c,unsigned imm) {
 uint64_t address=c->x[0];unsigned type=(uint32_t)c->x[1];
 uint32_t value=(uint32_t)c->x[2];int32_t count=(int32_t)c->x[3];
 if(address>=0x8000000000ull) return 0xd401;
 if(address&3) return 0xcc01;
 if(type>2) boundary("thread-address-arbitration-type",c,c->pc);
 threads_initialize();
 unsigned waiters=0;
 for(unsigned i=0;i<33;i++) if(guest_threads[i].status==THREAD_WAIT_ADDRESS && guest_threads[i].address_key==address) waiters++;
 /* Plain signal need not read or modify the userspace address. */
 if(imm==0x34 || type) {
  Region r=query(address);unsigned permission=1;
  if((imm==0x34 && type==1) || (imm==0x35 && (type==1 || !waiters || count<=0 || waiters<=(unsigned)count))) permission=3;
  if((r.permission&permission)!=permission || r.size-(address-r.base)<4) return 0xd401;
  uint32_t actual=LD32(address);
  if(imm==0x34) {
   if(type==2 ? actual!=value : (int32_t)actual>=(int32_t)value) return 0xfa01;
   if(type==1) ST32(address,actual-1u);
   int64_t ns=(int64_t)c->x[3];
   if(!ns) return 0xea01;
   guest_threads[current_thread].address_key=address;
   guest_threads[current_thread].address_deadline=thread_deadline(ns);
   guest_threads[current_thread].wait_order=++next_wait_order;
   guest_threads[current_thread].status=THREAD_WAIT_ADDRESS;
   (parallel_mode ? 0 : (thread_reschedule=1));return 0;
  }
  if(actual!=value) return 0xfa01;
  uint32_t updated=type==1 || !waiters ? value+1u : (count<=0 || waiters<=(unsigned)count ? value-1u : value);
  if(updated!=value) ST32(address,updated);
 }
 unsigned effective[33];thread_priorities(effective);
 for(unsigned n=0;n<waiters && (count<=0 || n<(unsigned)count);n++) {
  unsigned chosen=33;
  for(unsigned i=0;i<33;i++) if(guest_threads[i].status==THREAD_WAIT_ADDRESS && guest_threads[i].address_key==address &&
    (chosen==33 || effective[i]<effective[chosen] || (effective[i]==effective[chosen] && guest_threads[i].wait_order<guest_threads[chosen].wait_order))) chosen=i;
  guest_threads[chosen].status=THREAD_RUNNABLE;parallel_wake(chosen);
  guest_threads[chosen].address_key=guest_threads[chosen].address_deadline=0;
  thread_cpu(chosen)->x[0]=0;(parallel_mode ? 0 : (thread_reschedule=1));
 }
 return 0;
}
#include "execution_sample.h"
static uint64_t thread_checkpoint_step=~0ull;
/* Index of the first signaled handle, or -1. Threads signal once exited;
 * events stay signaled until cleared (polled without consuming). */
static int thread_objects_ready(const uint32_t *handles,unsigned count) {
 for(unsigned i=0;i<count;i++) {
  uint32_t h=handles[i];
  if(h==0x1000 || (h>=0x5000 && h<0x5020)) {
   unsigned t=h==0x1000 ? 0 : h-0x5000+1;
   if(guest_threads[t].status==THREAD_EXITED)return (int)i;
  } else if(h>=0x3000 && h<0x3040 && events[h-0x3000].handle) {
   DWORD r=WaitForSingleObject(events[h-0x3000].handle,0);
   if(r==WAIT_OBJECT_0)return (int)i;
   if(r!=WAIT_TIMEOUT)boundary("host-event-poll",&state,state.pc);
  } else boundary("wait-object-unimplemented",&state,state.pc);
 }
 return -1;
}
/* Profiler hook (nvn_hle.h): the scheduler is idle with nothing runnable. */
static volatile int thread_idle_flag;
static void thread_idle_marker(int idle) {thread_idle_flag=idle;}
/* Wake guest thread i if its wait is over (deadline, GPU fence, signaled object, sleep end); results go to its
 * live context. Returns 1 while it keeps waiting. */
static int thread_wake_check(unsigned i,uint64_t now) {
 unsigned status=guest_threads[i].status;
 if(status==THREAD_WAIT_ADDRESS && guest_threads[i].address_deadline) {
  if(now<guest_threads[i].address_deadline) return 1;
  guest_threads[i].status=THREAD_RUNNABLE;
  guest_threads[i].address_key=guest_threads[i].address_deadline=0;
  thread_cpu(i)->x[0]=0xea01;return 0;
 }
 if((status==THREAD_WAIT_CONDITION || status==THREAD_WAIT_MUTEX) && guest_threads[i].condition_deadline) {
  if(now<guest_threads[i].condition_deadline) return 1;
  guest_threads[i].status=THREAD_RUNNABLE;guest_threads[i].condition_key=0;
  guest_threads[i].condition_deadline=0;guest_threads[i].mutex=0;
  /* Timeout returns without the mutex; the guest SDK must reacquire it.
   * Preserve advisory waiter bits, as the kernel timeout path does. */
  thread_cpu(i)->x[0]=0xea01;return 0;
 }
 if(status==THREAD_WAIT_GPU) {
  if(!guest_threads[i].gpu_done(guest_threads[i].gpu_seq)) return 1;
  guest_threads[i].status=THREAD_RUNNABLE;return 0;
 }
 if(status==THREAD_WAIT_OBJECTS) {
  int ready=thread_objects_ready(guest_threads[i].wait_handles,guest_threads[i].wait_count);
  Cpu *t=thread_cpu(i);
  if(ready>=0) {guest_threads[i].status=THREAD_RUNNABLE;t->x[0]=0;t->x[1]=(uint64_t)ready;return 0;}
  if(guest_threads[i].wait_deadline && now>=guest_threads[i].wait_deadline) {
   guest_threads[i].status=THREAD_RUNNABLE;t->x[0]=0xea01;t->x[1]=0xffffffff;return 0;
  }
  return 1;
 }
 if(status==THREAD_SLEEPING) {
  if(now<guest_threads[i].wake_tick) return 1;
  guest_threads[i].status=THREAD_RUNNABLE;return 0;
 }
 return 0;
}
/* Parallel mode: the window's message queue belongs to the host thread that created it. */

static int app_window_owner(void) {return app_window_thread==GetCurrentThreadId();}
/* Parallel mode, kernel lock held: block until the calling guest thread is runnable. Waits on GPU fences and host
 * events are polled every millisecond; deadlines sleep until due; other threads wake it with parallel_wake. */
/* Wait statistics per guest thread and wait kind (AC_PARALLEL_STATS=1: printed every 2 s). */
static uint64_t parallel_wait_ticks[33][10],parallel_wait_count[33][10],parallel_mutex_ticks[33][33];
static void parallel_stats(void) {
 static int on=-1;static uint64_t last;
 if(on<0) on=GetEnvironmentVariableA("AC_PARALLEL_STATS",NULL,0)!=0;
 uint64_t now=rt_ticks();
 if(!on || now-last<2*19200000ull) return;
 last=now;
 printf("PARALLEL waits (ms, count) by thread/kind:");
 for(unsigned t=0;t<33;t++) for(unsigned k=0;k<10;k++) if(parallel_wait_count[t][k])
  printf(" t%u/s%u %.0f %llu",t,k,(double)parallel_wait_ticks[t][k]/19200.0,(unsigned long long)parallel_wait_count[t][k]);
 LARGE_INTEGER f;QueryPerformanceFrequency(&f);
 printf(" | lock contended %llu times, %.1f ms\n",(unsigned long long)klock_contended,1000.0*(double)klock_contended_qpc/(double)f.QuadPart);
 printf("PARALLEL nvn lock contended %llu times, %.1f ms; mutex waits (waiter<-owner ms):",(unsigned long long)nvn_contended,
  1000.0*(double)nvn_contended_qpc/(double)f.QuadPart);
 for(unsigned a=0;a<33;a++) for(unsigned b=0;b<33;b++) if(parallel_mutex_ticks[a][b]>19200*20)
  printf(" t%u<-t%u %.0f",a,b,(double)parallel_mutex_ticks[a][b]/19200.0);
 printf("; nvn time (ms, calls):");
 for(unsigned t=0;t<33;t++) if(nvn_thread_calls[t])
  printf(" t%u %.0f %llu",t,1000.0*(double)nvn_thread_qpc[t]/(double)f.QuadPart,(unsigned long long)nvn_thread_calls[t]);
 memset(nvn_thread_qpc,0,sizeof(nvn_thread_qpc));memset(nvn_thread_calls,0,sizeof(nvn_thread_calls));
 puts("");
 klock_contended=klock_contended_qpc=nvn_contended=nvn_contended_qpc=0;memset(parallel_mutex_ticks,0,sizeof(parallel_mutex_ticks));
 memset(parallel_wait_ticks,0,sizeof(parallel_wait_ticks));memset(parallel_wait_count,0,sizeof(parallel_wait_count));
}
static void parallel_block_wait(void);
static void parallel_block(void) {
 unsigned self=current_thread,kind=guest_threads[self].status,owner=guest_threads[self].mutex_owner;
 uint64_t start=rt_ticks();
 parallel_block_wait();
 if(kind<10) {parallel_wait_ticks[self][kind]+=rt_ticks()-start;parallel_wait_count[self][kind]++;}
 if(kind==THREAD_WAIT_MUTEX && owner<33) parallel_mutex_ticks[self][owner]+=rt_ticks()-start;
 parallel_stats();
}
static void parallel_block_wait(void) {
 unsigned self=current_thread;
 for(;;) {
  unsigned status=guest_threads[self].status;
  if(status==THREAD_RUNNABLE || status==THREAD_EXITED) return;
  uint64_t now=rt_ticks();
  if(!thread_wake_check(self,now) && guest_threads[self].status==THREAD_RUNNABLE) return;
  status=guest_threads[self].status;
  uint64_t deadline=status==THREAD_WAIT_ADDRESS ? guest_threads[self].address_deadline :
                    status==THREAD_WAIT_CONDITION || status==THREAD_WAIT_MUTEX ? guest_threads[self].condition_deadline :
                    status==THREAD_SLEEPING ? guest_threads[self].wake_tick : 0;
  DWORD ms=20;
  if(status==THREAD_WAIT_GPU || status==THREAD_WAIT_OBJECTS) ms=1;
  else if(deadline) {uint64_t left=deadline>now ? (deadline-now)/19200 : 0;ms=left<1 ? 1 : left>20 ? 20 : (DWORD)left;}
  if(app_window_owner()) {app_checkpoint();if(ms>4) ms=4;}  /* keep the window responsive while blocked */
  klock_wait(self,ms);
 }
}
/* Parallel mode: each started guest thread gets a host thread running the dispatcher on its own context. */
static void (*guest_execute)(Cpu *c);
static void prof_attach(unsigned thread);
static DWORD WINAPI parallel_thread_main(LPVOID parameter) {
 unsigned i=(unsigned)(uintptr_t)parameter;
 current_thread=i;state=guest_threads[i].cpu;  /* (initial context from svcCreateThread) */
 klock();live_cpu[i]=&state;kunlock();
 prof_attach(i);
 guest_execute(&state);  /* returns only at the host return sentinel; guest threads end with svcExitThread */
 klock();guest_threads[i].status=THREAD_EXITED;kunlock();
 return 0;
}
static void parallel_start(unsigned i) {
 /* 16 MB reserved: translated page functions nest along guest calls */
 HANDLE h=CreateThread(NULL,16u<<20,parallel_thread_main,(LPVOID)(uintptr_t)i,STACK_SIZE_PARAM_IS_A_RESERVATION,NULL);
 if(!h) boundary("parallel-thread-create",&state,state.pc);
 printf("PARALLEL guest thread %u on host thread %lu\n",i,(unsigned long)GetThreadId(h));
 CloseHandle(h);
}
/* Nonzero while host code runs a nested guest callback on this host stack. */
static __thread unsigned thread_switch_locked;
static int thread_checkpoint(Cpu *c,uint64_t step) {
 if(parallel_mode) {  /* no context switching: only the window thread's housekeeping */
  if(app_window_owner() && !thread_switch_locked) {klock();app_checkpoint();kunlock();}
  return 0;
 }
 if(thread_switch_locked) {
  if(guest_threads[current_thread].status!=THREAD_RUNNABLE)boundary("thread-switch-in-host-callback",c,c->pc);
  return 0;
 }
 if(!thread_reschedule && thread_checkpoint_step==step) return 0;
 thread_checkpoint_step=step;
 execution_sample(c,step,current_thread);
 app_checkpoint();threads_initialize();thread_reschedule=0;
 for(;;) {
 uint64_t now=rt_ticks();unsigned sleeping=0;
 for(unsigned i=0;i<33;i++) sleeping+=thread_wake_check(i,now);
 unsigned chosen=33,core=guest_threads[current_thread].core,effective[33];thread_priorities(effective);
 for(unsigned turn=1;turn<=4 && chosen==33;turn++) {
  unsigned candidate_core=(core+turn)%4,best=64;
  for(unsigned offset=1;offset<=33;offset++) {
   unsigned i=(current_thread+offset)%33;
   if(guest_threads[i].status==THREAD_RUNNABLE && guest_threads[i].core==candidate_core && effective[i]<best) {
    chosen=i;best=effective[i];
   }
  }
 }
 if(chosen==33) {
  if(!sleeping) boundary("thread-no-runnable-context",c,c->pc);
  thread_idle_marker(1);
  DWORD idle_result=MsgWaitForMultipleObjectsEx(0,NULL,1,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
  thread_idle_marker(0);
  if(idle_result==WAIT_FAILED)
   boundary("thread-host-sleep",c,c->pc);
  app_checkpoint();continue;
 }
 if(chosen==current_thread) return 0;
 rt_clear_pair_monitor(c);c->excl_addr=~0ull;
 guest_threads[current_thread].cpu=*c;
 current_thread=chosen;*c=guest_threads[chosen].cpu;
 c->excl_addr=~0ull;
 return 1;
 }
}
static int thread_svc(Cpu *c,unsigned imm) {
 if(imm==0x34 || imm==0x35) {c->x[0]=thread_address_svc(c,imm);return 1;}
 if(imm==0x10) {threads_initialize();c->x[0]=guest_threads[current_thread].core;return 1;}
 if(imm==0x1c) {
  threads_initialize();uint64_t address=c->x[0],key=c->x[1];uint32_t tag=(uint32_t)c->x[2];
  thread_mutex_memory(address);thread_mutex_memory(key);
  if(thread_index(tag)!=current_thread) boundary("condition-wait-thread-tag",c,c->pc);
  int64_t ns=(int64_t)c->x[3];
  /* No guest instructions can interleave release, waiter registration and
   * blocking: all three happen within this serialized syscall. */
  ST32(key,1);thread_mutex_unlock(address);
  c->x[0]=ns==0 ? 0xea01 : 0;
  if(ns!=0) {
   guest_threads[current_thread].mutex=address;guest_threads[current_thread].mutex_tag=tag;
   guest_threads[current_thread].condition_key=key;
   guest_threads[current_thread].condition_deadline=thread_deadline(ns);
   guest_threads[current_thread].wait_order=++next_wait_order;
   guest_threads[current_thread].status=THREAD_WAIT_CONDITION;
  }
  return 1;
 }
 if(imm==0x1d) {thread_condition_signal(c->x[0],(int32_t)c->x[1]);c->x[0]=0;return 1;}
 if(imm==0xb) {
  threads_initialize();int64_t ns=(int64_t)c->x[0];
  /* 0: yield without migration, -1: with migration, -2: to any thread. The
   * bring-up scheduler rotates virtual cores, so all three reschedule. */
  if(ns<-2) boundary("thread-yield-mode",c,c->pc);
  if(ns>0) {
   guest_threads[current_thread].wake_tick=rt_ticks()+(uint64_t)(((__uint128_t)(uint64_t)ns*19200000+999999999)/1000000000);
   guest_threads[current_thread].status=THREAD_SLEEPING;
  }
  (parallel_mode ? 0 : (thread_reschedule=1));c->x[0]=0;return 1;
 }
 if(imm==0x1a) {
  thread_mutex_memory(c->x[1]);threads_initialize();
  uint32_t owner=(uint32_t)c->x[0],tag=(uint32_t)c->x[2];
  if(LD32(c->x[1])==(owner|0x40000000u)) {
   unsigned i=thread_index(owner);
   if(thread_index(tag)!=current_thread || i==current_thread || guest_threads[i].status==THREAD_EXITED)
    boundary("thread-mutex-owner",c,c->pc);
   guest_threads[current_thread].mutex=c->x[1];guest_threads[current_thread].mutex_tag=tag;
   guest_threads[current_thread].mutex_owner=i;guest_threads[current_thread].status=THREAD_WAIT_MUTEX;
   guest_threads[current_thread].condition_deadline=0;guest_threads[current_thread].wait_order=++next_wait_order;
   (parallel_mode ? 0 : (thread_reschedule=1));
  }
  c->x[0]=0;return 1;
 }
 if(imm==0x1b) {thread_mutex_unlock(c->x[0]);c->x[0]=0;return 1;}
 if(imm==0xe) {
  unsigned i=thread_index(c->x[2]);
  c->x[1]=(uint32_t)guest_threads[i].ideal_core;c->x[2]=guest_threads[i].affinity;c->x[0]=0;return 1;
 }
 if(imm==0xf) {
  unsigned i=thread_index(c->x[0]);int core=(int32_t)c->x[1];uint64_t mask=c->x[2];
  if(core==-2) {core=MAIN_CORE;mask=1ull<<MAIN_CORE;}
  if(core==-3) core=guest_threads[i].ideal_core;
  if(!mask || (mask&~PROCESS_CORE_MASK) || core < -1 || core>3 || (core>=0 && !(mask&(1ull<<core))))
   boundary("thread-core-mask",c,c->pc);
  guest_threads[i].affinity=mask;guest_threads[i].ideal_core=core;
  if(!(mask&(1ull<<guest_threads[i].core))) guest_threads[i].core=core>=0 ? (unsigned)core : (unsigned)__builtin_ctzll(mask);
  (parallel_mode ? 0 : (thread_reschedule=1));c->x[0]=0;return 1;
 }
 if(imm==8) {c->x[0]=thread_create(c);return 1;}
 if(imm==9) {
  unsigned i=thread_index(c->x[0]);
  if(guest_threads[i].status!=THREAD_CREATED) boundary("thread-start-state",c,c->pc);
  guest_threads[i].status=THREAD_RUNNABLE;(parallel_mode ? 0 : (thread_reschedule=1));c->x[0]=0;
  if(parallel_mode) parallel_start(i);
  return 1;
 }
 if(imm==10) {
  threads_initialize();guest_threads[current_thread].status=THREAD_EXITED;
  (parallel_mode ? 0 : (thread_reschedule=1));return 1;
 }
 if(imm==0xc || imm==0x25) {
  unsigned i=thread_index(c->x[1]);
  c->x[1]=imm==0xc ? guest_threads[i].priority : guest_threads[i].id;c->x[0]=0;return 1;
 }
 if(imm==0x16 && (c->x[0]==0x1000 || (c->x[0]>=0x5000 && c->x[0]<0x5020))) {
  unsigned i=thread_index(c->x[0]);
  /* Retain running objects after handle closure; exited objects can be freed.
   * A closed live object is intentionally retained within the bounded pool. */
  guest_threads[i].handle_open=0;
  if(i && (guest_threads[i].status==THREAD_CREATED || guest_threads[i].status==THREAD_EXITED)) {
   if(i==current_thread) boundary("thread-close-current-context",c,c->pc);
   if(!VirtualFree((void*)(uintptr_t)thread_tls[i-1],0,MEM_RELEASE)) boundary("thread-tls-release",c,c->pc);
   thread_tls[i-1]=0;memset(&guest_threads[i],0,sizeof(guest_threads[i]));
  }
  c->x[0]=0;return 1;
 }
 return 0;
}
