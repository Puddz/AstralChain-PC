/* Bounded Maxwell macro execution. Addresses are instruction/method WORDS.
 * ISA field reference: envytools envydis/macro.c. No external implementation
 * is embedded here. Unresolved corner cases reject rather than guess. */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct {
 uint32_t regs[8],parameter;
 unsigned pc,steps,method,increment,method_set;
 unsigned carry,carry_valid,has_parameter,waiting,done;
 unsigned branch_pending,branch_target,exit_pending;
} GpuMacro;
typedef const char *(*GpuMacroRead)(void*,unsigned,uint32_t*);
typedef const char *(*GpuMacroEmit)(void*,unsigned,uint32_t);
static void gpu_macro_begin(GpuMacro *macro,unsigned pc,uint32_t argument) {
 *macro=(GpuMacro){0};macro->pc=pc;macro->regs[1]=argument;
}
/* One instruction commits only after all callbacks succeed. Read must have no
 * side effects; emit must leave its target unchanged when rejecting a write. */
static const char *gpu_macro_step(GpuMacro *macro,const uint32_t *code,const uint8_t *valid,
 unsigned capacity,GpuMacroRead read,GpuMacroEmit emit,void *context) {
 if(macro->done) return "macro already finished";
 if(macro->pc>=capacity || !valid[macro->pc]) return "macro instruction not uploaded";
 if(macro->steps>=65536) return "macro instruction budget";
 GpuMacro next=*macro;next.waiting=0;
 uint32_t word=code[macro->pc],x=macro->regs[(word>>11)&7],y=macro->regs[(word>>14)&7];
 unsigned op=word&7,assignment=(word>>4)&7,dst=(word>>8)&7;
 int32_t immediate=(int32_t)(word>>14);if(immediate&0x20000) immediate-=0x40000;
 if(word&8) return "reserved macro bit";
 unsigned delayed=macro->branch_pending || macro->exit_pending;
 next.pc=macro->branch_pending ? macro->branch_target : macro->pc+1;
 next.branch_pending=0;next.exit_pending=0;
 if(op==7) {
  if(delayed) return "macro branch in delay slot";
  if(word&0x740) return "reserved macro branch bits";
  int taken=(word&16) ? x!=0 : x==0;
  if(taken) {
   int64_t target=(int64_t)macro->pc+immediate;
   if(target<0 || target>=capacity) return "macro branch range";
   if(word&32) next.pc=(unsigned)target;
   else {next.branch_pending=1;next.branch_target=(unsigned)target;}
  } else if(word&128) next.exit_pending=1;
 } else {
  int needs_parameter=assignment==0 || assignment==3 || assignment==5 || assignment==6;
  if(needs_parameter && !macro->has_parameter) {macro->waiting=1;return NULL;}
  uint32_t result=0;
  if(op==0) {
   if(word&0xffc00000u) return "reserved macro ALU bits";
   unsigned alu=(word>>17)&31;uint64_t wide;
   switch(alu) {
    case 0:case 1:
     if(alu==1 && !macro->carry_valid) return "macro carry not established";
     wide=(uint64_t)x+y+(alu==1 ? macro->carry : 0);
     result=(uint32_t)wide;next.carry=(unsigned)(wide>>32);next.carry_valid=1;break;
    /* SUB's low word is established, but sources disagree on borrow polarity.
     * Invalidate the flag so a later ADC cannot consume stale ADD carry. */
    case 2:result=x-y;next.carry=0;next.carry_valid=0;break;
    case 3:return "macro subtract carry semantics unresolved";
    case 8:result=x^y;break;
    case 9:result=x|y;break;
    case 10:result=x&y;break;
    case 11:result=x&~y;break;
    case 12:result=~(x&y);break;
    default:return "unsupported macro ALU operation";
   }
  } else if(op==1) result=x+(uint32_t)immediate;
  else if(op>=2 && op<=4) {
   unsigned source=(word>>17)&31,size=(word>>22)&31,target=(word>>27)&31;
   uint32_t mask=(1u<<size)-1;
   if(op==3) {if(x>31) return "macro variable shift range unresolved";source=x;}
   if(op==4) {if(x>31) return "macro variable shift range unresolved";target=x;}
   result=((y>>source)&mask)<<target;
   if(op==2) result|=x&~(mask<<target);
  } else if(op==5) {
   uint32_t address=x+(uint32_t)immediate;
   if(address>4095) return "macro read range";
   const char *error=read(context,address,&result);if(error) return error;
  } else return "unsupported macro opcode";
  uint32_t stored=needs_parameter && assignment!=6 ? macro->parameter : result;
  if(dst) next.regs[dst]=stored;
  if(assignment==2 || assignment==5 || assignment==6 || assignment==7) {
   next.method=result&4095;next.increment=(result>>12)&63;next.method_set=1;
  }
  if(assignment==3 || assignment==4 || assignment==6 || assignment==7) {
   if(!next.method_set || next.method>4095) return "macro method address unavailable";
   uint32_t value=assignment==6 ? macro->parameter : assignment==7 ? (result>>12)&63 : result;
   const char *error=emit(context,next.method,value);if(error) return error;
   /* The initial address and six-bit immediate payload are unambiguous even
    * when references disagree on increment width. Permit that first send,
    * then invalidate the address until another maddr explicitly replaces it. */
   if(next.increment>15) next.method_set=0;
   else next.method+=next.increment;
  }
  if(needs_parameter) next.has_parameter=0;
  /* Taken branches suppress exit in their delay slot. */
  if((word&128) && !delayed) next.exit_pending=1;
 }
 if(macro->exit_pending) next.done=1;
 next.steps++;*macro=next;return NULL;
}
static const char *gpu_macro_run(GpuMacro *macro,const uint32_t *code,const uint8_t *valid,
 unsigned capacity,GpuMacroRead read,GpuMacroEmit emit,void *context) {
 while(!macro->done) {
  const char *error=gpu_macro_step(macro,code,valid,capacity,read,emit,context);
  if(error || macro->waiting) return error;
 }
 if(macro->has_parameter) return "macro unused parameter";
 return NULL;
}
