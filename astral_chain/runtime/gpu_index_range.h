/* Maxwell indexed-draw input range, before any host memory access.
 * NVIDIA clb197.h index A..F; limit is the last accessible byte. */
#pragma once
#include "gpu_graphics_state.h"
typedef struct {uint64_t address,size;uint32_t count,first;unsigned width;} GpuIndexRange;
static const char *gpu_index_range(const GpuGraphicsState *g,uint32_t count,GpuIndexRange *out) {
 for(unsigned i=0;i<6;i++)if(!g->valid[0x17c8/4+i])return "index descriptor unknown";
 uint32_t upper=g->values[0x17c8/4],limit_upper=g->values[0x17d0/4],format=g->values[0x17d8/4];
 if(upper>255 || limit_upper>255 || format>2)return "invalid index descriptor";
 uint64_t base=((uint64_t)upper<<32)|g->values[0x17cc/4];
 uint64_t limit=((uint64_t)limit_upper<<32)|g->values[0x17d4/4];
 unsigned width=1u<<format;uint32_t first=g->values[0x17dc/4];
 uint64_t offset=(uint64_t)first*width,size=(uint64_t)count*width;
 if(base>limit || offset>limit-base || (size && size-1>limit-base-offset))return "index range exceeds limit";
 GpuIndexRange value={base+offset,size,count,first,width};*out=value;return NULL;
}
