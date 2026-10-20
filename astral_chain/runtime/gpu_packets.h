/* Maxwell DMA method packets. Methods are word indices, not byte offsets.
 * State survives descriptor boundaries. A rejected callback consumes no word. */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { unsigned method,subchannel,remaining,mode; } GpuPacketState;
typedef int (*GpuMethodVisitor)(void*,unsigned,unsigned,uint32_t);
static const char *gpu_packet_word(GpuPacketState *state,uint32_t word,GpuMethodVisitor visit,void *context) {
 GpuPacketState next=*state;
 if(next.remaining) {
  if(!visit(context,next.subchannel,next.method,word)) return "method rejected";
  next.remaining--;
  if(next.mode==1 || next.mode==5) {next.method++;if(next.mode==5) next.mode=3;}
  *state=next;return NULL;
 }
 if(!word) return NULL; /* zero-length legacy increment packet / NOP */
 unsigned mode=word>>29,method=word&4095,subchannel=(word>>13)&7,count=(word>>16)&8191;
 if(word&0x1000) return "reserved method bit";
 if(mode!=1 && mode!=3 && mode!=4 && mode!=5) return "unsupported packet opcode";
 if(mode==4) return visit(context,subchannel,method,count) ? NULL : "method rejected";
 if(count && ((mode==1 && count>4096-method) || (mode==5 && count>1 && method==4095))) return "method range overflow";
 *state=(GpuPacketState){method,subchannel,count,mode};return NULL;
}
