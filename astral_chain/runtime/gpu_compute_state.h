/* MAXWELL_COMPUTE_B configuration, independent of 3D state. Field definitions:
 * NVIDIA open-gpu-doc classes/compute/clb1c0.h. No dispatch is implemented. */
#pragma once
#include <stdint.h>
#include <stddef.h>
enum { GPU_COMPUTE_EXCEPTIONS, GPU_COMPUTE_BINDLESS_SLOT, GPU_COMPUTE_LOCAL_WINDOW,
 GPU_COMPUTE_SHARED_WINDOW, GPU_COMPUTE_PROGRAM_HIGH, GPU_COMPUTE_PROGRAM_LOW,
 GPU_COMPUTE_ISA, GPU_COMPUTE_LOCAL_HIGH, GPU_COMPUTE_LOCAL_LOW,
 GPU_COMPUTE_NON_THROTTLED_HIGH, GPU_COMPUTE_NON_THROTTLED_LOW, GPU_COMPUTE_NON_THROTTLED_SMS,
 GPU_COMPUTE_THROTTLED_HIGH, GPU_COMPUTE_THROTTLED_LOW, GPU_COMPUTE_THROTTLED_SMS,
 GPU_COMPUTE_SAMPLER_HIGH, GPU_COMPUTE_SAMPLER_LOW, GPU_COMPUTE_SAMPLER_LIMIT,
 GPU_COMPUTE_HEADER_HIGH, GPU_COMPUTE_HEADER_LOW, GPU_COMPUTE_HEADER_LIMIT, GPU_COMPUTE_FIELDS };
typedef struct {uint32_t values[GPU_COMPUTE_FIELDS];unsigned valid,dirty;} GpuComputeState;
static const char *gpu_compute_write(GpuComputeState *compute,unsigned method,uint32_t value) {
 unsigned field;uint32_t mask;
 switch(method*4) {
  case 0x0790:field=GPU_COMPUTE_LOCAL_HIGH;mask=255;break;
  case 0x0794:field=GPU_COMPUTE_LOCAL_LOW;mask=0xffffffffu;break;
  case 0x02e4:field=GPU_COMPUTE_NON_THROTTLED_HIGH;mask=255;break;
  case 0x02e8:field=GPU_COMPUTE_NON_THROTTLED_LOW;mask=0xffffffffu;break;
  case 0x02ec:field=GPU_COMPUTE_NON_THROTTLED_SMS;mask=511;break;
  case 0x02f0:field=GPU_COMPUTE_THROTTLED_HIGH;mask=255;break;
  case 0x02f4:field=GPU_COMPUTE_THROTTLED_LOW;mask=0xffffffffu;break;
  case 0x02f8:field=GPU_COMPUTE_THROTTLED_SMS;mask=511;break;
  case 0x155c:field=GPU_COMPUTE_SAMPLER_HIGH;mask=255;break;
  case 0x1560:field=GPU_COMPUTE_SAMPLER_LOW;mask=0xffffffffu;break;
  case 0x1564:field=GPU_COMPUTE_SAMPLER_LIMIT;mask=0xfffff;break;
  case 0x1574:field=GPU_COMPUTE_HEADER_HIGH;mask=255;break;
  case 0x1578:field=GPU_COMPUTE_HEADER_LOW;mask=0xffffffffu;break;
  case 0x157c:field=GPU_COMPUTE_HEADER_LIMIT;mask=0x3fffff;break;
  case 0x1528:field=GPU_COMPUTE_EXCEPTIONS;mask=1;break;
  case 0x2608:field=GPU_COMPUTE_BINDLESS_SLOT;mask=7;break;
  case 0x077c:field=GPU_COMPUTE_LOCAL_WINDOW;mask=0xffffffffu;break;
  case 0x0214:field=GPU_COMPUTE_SHARED_WINDOW;mask=0xffffffffu;break;
  case 0x1608:field=GPU_COMPUTE_PROGRAM_HIGH;mask=255;break;
  case 0x160c:field=GPU_COMPUTE_PROGRAM_LOW;mask=0xffffffffu;break;
  case 0x0310:field=GPU_COMPUTE_ISA;mask=65535;break;
  default:return "unsupported compute method";
 }
 if(value&~mask) return "unsupported compute state bits";
 if(field==GPU_COMPUTE_ISA && value!=0x503) return "unsupported compute shader ISA";
 unsigned bit=1u<<field;
 if(!(compute->valid&bit) || compute->values[field]!=value) compute->dirty|=bit;
 compute->values[field]=value;compute->valid|=bit;return NULL;
}
