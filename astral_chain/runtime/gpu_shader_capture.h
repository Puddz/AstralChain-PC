/* Diagnostic windows only: a shader's true instruction extent is not inferred.
 * Included after GPU mapping and engine types, and host memory query helpers. */
#pragma once
#include "gpu_index_range.h"
#include "gpu_texture_capture.h"
enum { GPU_SHADER_WINDOW_SIZE=4096 };
/* Versioned little-endian capture: header, register words, then validity bytes.
 * No C struct padding or unknown-register reset guesses are serialized. */
static void gpu_capture_graphics_state(const GpuGraphicsState *g) {
 const uint32_t header[]={0x53474341,1,GPU_GRAPHICS_METHODS,0};
 HANDLE file=CreateFileA("gpu-graphics-state.bin",GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
 if(file==INVALID_HANDLE_VALUE){printf("GPU state capture create-error=%lu\n",GetLastError());return;}
 const void *parts[]={header,g->values,g->valid};
 const DWORD sizes[]={sizeof(header),sizeof(g->values),sizeof(g->valid)};
 unsigned complete=1;DWORD total=0;
 for(unsigned i=0;i<3;i++) {
  DWORD written=0;BOOL ok=WriteFile(file,parts[i],sizes[i],&written,NULL);total+=written;
  if(!ok || written!=sizes[i]){complete=0;break;}
 }
 if(!CloseHandle(file))complete=0;
 printf("GPU state capture bytes=%lu complete=%u\n",total,complete);
}
/* Diagnostic only. A rejected capture cannot authorize execution of a draw. */
static void gpu_capture_pitch_range(void *opaque,uint64_t address,uint64_t size,const char *path) {
 printf("GPU draw input file=%s address=0x%llx bytes=%llu\n",path,(unsigned long long)address,(unsigned long long)size);
 if(!opaque || !size || size>4*1024*1024 || address>=(1ull<<40) || size>(1ull<<40)-address) {
  printf("GPU draw input unavailable=extent or diagnostic budget\n");return;
 }
 GpuMemoryView view=gpu_resolve((GpuAddressSpace*)opaque,address,size);
 if(!view.bytes || view.kind || view.storage!=GPU_STORAGE_CANONICAL) {printf("GPU draw input unavailable=mapping\n");return;}
 uint64_t cpu=(uint64_t)(uintptr_t)view.bytes;Region r=query(cpu);
 if(!(r.permission&1) || cpu<r.base || cpu-r.base>=r.size || size>r.size-(cpu-r.base)) {
  printf("GPU draw input unavailable=backing permissions\n");return;
 }
 HANDLE file=CreateFileA(path,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
 if(file==INVALID_HANDLE_VALUE) {printf("GPU draw input create-error=%lu\n",GetLastError());return;}
 DWORD written=0;BOOL ok=WriteFile(file,view.bytes,(DWORD)size,&written,NULL),closed=CloseHandle(file);
 printf("GPU draw input file=%s captured=%lu complete=%u\n",path,written,ok && closed && written==size);
}
static void gpu_capture_bound_buffers(const GpuEngineState *engine) {
 const GpuGraphicsState *g=&engine->graphics;char path[96];
 gpu_capture_graphics_state(g);
 const char *texture_error=gpu_capture_first_texture(engine);
 if(texture_error)printf("GPU first texture unavailable=%s\n",texture_error);
 const unsigned pools[]={0x1574/4,0x155c/4};
 const unsigned maximums[]={0x3fffff,0xfffff};
 const char *pool_files[]={"gpu-texture-headers.bin","gpu-sampler-descriptors.bin"};
 for(unsigned pool=0;pool<2;pool++) {
  unsigned base=pools[pool];
  if(!g->valid[base] || !g->valid[base+1] || !g->valid[base+2]) {
   printf("GPU descriptor pool file=%s unavailable=unknown registers\n",pool_files[pool]);continue;
  }
  if(g->values[base]>255 || g->values[base+2]>maximums[pool]) {
   printf("GPU descriptor pool file=%s unavailable=invalid fields\n",pool_files[pool]);continue;
  }
  uint64_t address=((uint64_t)g->values[base]<<32)|g->values[base+1];
  uint64_t entries=(uint64_t)g->values[base+2]+1;
  printf("GPU descriptor pool file=%s entries=%llu stride=32\n",pool_files[pool],(unsigned long long)entries);
  gpu_capture_pitch_range(engine->memory_context,address,entries*32,pool_files[pool]);
 }
 for(unsigned slot=0;slot<32;slot++) {
  unsigned base=0x1c00/4+slot*4,limit=0x1f00/4+slot*2;
  if(!g->valid[base] || !(g->values[base]&0x1000))continue;
  if(!g->valid[base+1] || !g->valid[base+2] || !g->valid[limit] || !g->valid[limit+1]) {
   printf("GPU vertex capture slot=%u unavailable=unknown descriptor\n",slot);continue;
  }
  uint64_t start=((uint64_t)g->values[base+1]<<32)|g->values[base+2];
  uint64_t end=((uint64_t)g->values[limit]<<32)|g->values[limit+1];
  if(end<start || end>=(1ull<<40)) {printf("GPU vertex capture slot=%u unavailable=invalid limit\n",slot);continue;}
  printf("GPU vertex descriptor slot=%u stride=%u format=0x%x\n",slot,g->values[base]&0xfff,g->values[base]);
  snprintf(path,sizeof(path),"gpu-vertex-stream%u.bin",slot);
  gpu_capture_pitch_range(engine->memory_context,start,end-start+1,path);
 }
 for(unsigned stage=0;stage<5;stage++)for(unsigned slot=0;slot<32;slot++) {
  if(!engine->constant_buffers[stage][slot].known || !engine->constant_buffers[stage][slot].enabled)continue;
  snprintf(path,sizeof(path),"gpu-cbuf-stage%u-slot%u.bin",stage,slot);
  gpu_capture_pitch_range(engine->memory_context,engine->constant_buffers[stage][slot].address,
   engine->constant_buffers[stage][slot].size,path);
 }
}
static void gpu_capture_index_window(void *opaque,const GpuGraphicsState *g,uint32_t count) {
 GpuIndexRange range;const char *error=gpu_index_range(g,count,&range);
 if(error) {printf("GPU index capture unavailable=%s\n",error);return;}
 printf("GPU index range address=0x%llx bytes=%llu width=%u count=%u first=%u\n",
  (unsigned long long)range.address,(unsigned long long)range.size,range.width,count,range.first);
 if(!range.size)return;
 if(!opaque || range.size>4096) {printf("GPU index capture unavailable=address space or diagnostic budget\n");return;}
 GpuMemoryView view=gpu_resolve((GpuAddressSpace*)opaque,range.address,range.size);
 if(!view.bytes || view.kind || view.storage!=GPU_STORAGE_CANONICAL) {printf("GPU index capture unavailable=mapping\n");return;}
 uint64_t cpu=(uint64_t)(uintptr_t)view.bytes;Region r=query(cpu);
 if(!(r.permission&1) || cpu<r.base || cpu-r.base>=r.size || range.size>r.size-(cpu-r.base)) {
  printf("GPU index capture unavailable=backing permissions\n");return;
 }
 HANDLE file=CreateFileA("gpu-first-index-data.bin",GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
 if(file==INVALID_HANDLE_VALUE) {printf("GPU index capture create-error=%lu\n",GetLastError());return;}
 DWORD written=0;BOOL ok=WriteFile(file,view.bytes,(DWORD)range.size,&written,NULL),closed=CloseHandle(file);
 printf("GPU index capture bytes=%lu complete=%u\n",written,ok && closed && written==range.size);
}
static const char *gpu_shader_window(GpuAddressSpace *space,const GpuGraphicsState *g,
 unsigned slot,uint64_t *address,uint8_t *bytes) {
 if(!space || slot>=6)return "shader address space or slot unavailable";
 unsigned program=0x2000/4+slot*16;
 if(!g->valid[0x1608/4] || !g->valid[0x160c/4] || !g->valid[program+1])return "shader address unknown";
 uint64_t base=((uint64_t)g->values[0x1608/4]<<32)|g->values[0x160c/4];
 uint64_t offset=g->values[program+1],limit=1ull<<40;
 if(base>=limit || offset>limit-base || GPU_SHADER_WINDOW_SIZE>limit-base-offset)return "shader address overflow";
 uint64_t gpu=base+offset;
 GpuMemoryView view=gpu_resolve(space,gpu,GPU_SHADER_WINDOW_SIZE);
 if(!view.bytes || view.kind || view.storage!=GPU_STORAGE_CANONICAL)return "shader window unmapped or non-pitch";
 uint64_t cpu=(uint64_t)(uintptr_t)view.bytes;Region r=query(cpu);
 if(!(r.permission&1) || cpu<r.base || cpu-r.base>=r.size || GPU_SHADER_WINDOW_SIZE>r.size-(cpu-r.base))
  return "shader window backing not readable";
 memcpy(bytes,view.bytes,GPU_SHADER_WINDOW_SIZE);*address=gpu;return NULL;
}
static void gpu_capture_shader_windows(const GpuEngineState *engine) {
 uint8_t bytes[GPU_SHADER_WINDOW_SIZE];
 for(unsigned slot=0;slot<6;slot++) {
  if(!engine->graphics.valid[0x2000/4+slot*16+1])continue;
  uint64_t address=0;
  const char *error=gpu_shader_window((GpuAddressSpace*)engine->memory_context,&engine->graphics,slot,&address,bytes);
  if(error) {printf("GPU shader window slot=%u unavailable=%s\n",slot,error);continue;}
  char path[96];snprintf(path,sizeof(path),"gpu-shader-window-slot%u-%010llx.bin",slot,(unsigned long long)address);
  HANDLE file=CreateFileA(path,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
  if(file==INVALID_HANDLE_VALUE) {printf("GPU shader window slot=%u create-error=%lu\n",slot,GetLastError());continue;}
  DWORD written=0;BOOL ok=WriteFile(file,bytes,sizeof(bytes),&written,NULL);BOOL closed=CloseHandle(file);
  printf("GPU shader window slot=%u address=0x%llx size=%u file=%s complete=%u\n",slot,
   (unsigned long long)address,(unsigned)written,path,ok && closed && written==sizeof(bytes));
 }
}
