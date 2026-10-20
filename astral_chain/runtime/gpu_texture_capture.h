/* Diagnostic for the observed fragment TEXS handle at cbuf byte 0x20.
 * Not a draw backend. Layout fields follow deko3d Maxwell TIC definitions. */
#pragma once
typedef struct { uint64_t address,size;unsigned width,height,block_height; } GpuTextureExtent;
static const char *gpu_texture_extent(const uint32_t *w,GpuTextureExtent *out) {
 if(w[0]!=0x76da4a29u || ((w[2]>>21)&7)!=3 || ((w[4]>>23)&15)!=1)
  return "unsupported texture format or header";
 if((w[2]&0xe01f0000u) || (w[3]&0xf000ffffu)!=((w[3]>>3&7)<<3) ||
    (w[4]&0x00470000u) || (w[5]&0x7fff0000u) || !(w[5]&0x80000000u) || w[7])
  return "unsupported texture layers, tiles, mip or sample layout";
 GpuTextureExtent t={0};t.address=((uint64_t)(w[2]&65535)<<32)|w[1];
 t.width=(w[4]&65535)+1;t.height=(w[5]&65535)+1;t.block_height=(w[3]>>3)&7;
 if(t.width>16384 || t.height>16384 || t.block_height>5 || (t.address&511))
  return "texture dimensions or alignment";
 uint64_t rows=8ull<<t.block_height,pitch=((uint64_t)t.width*4+63)&~63ull;
 t.size=((t.height+rows-1)&~(rows-1))*pitch;
 if(t.size>16*1024*1024 || t.address>=(1ull<<40) || t.size>(1ull<<40)-t.address)
  return "texture extent or diagnostic budget";
 *out=t;return NULL;
}
/* Split on mapping page boundaries so kind aliases may change within an image.
 * Canonical storage is mandatory; this never decompresses hardware data. */
static const char *gpu_capture_read(GpuAddressSpace *space,uint64_t address,uint64_t size,
 void *destination,unsigned surface) {
 if(!space || address>=(1ull<<40) || size>(1ull<<40)-address)return "capture address range";
 for(uint64_t done=0;done<size;) {
  uint64_t at=address+done,n=4096-(at&4095);if(n>size-done)n=size-done;
  GpuMemoryView v=gpu_resolve(space,at,n);
  if(!v.bytes || v.storage!=GPU_STORAGE_CANONICAL ||
     (v.kind && (!surface || (v.kind!=0xfe && v.kind!=0x51))))return "capture mapping or storage";
  uint64_t cpu=(uint64_t)(uintptr_t)v.bytes;Region r=query(cpu);
  if(!(r.permission&1) || cpu<r.base || cpu-r.base>=r.size || n>r.size-(cpu-r.base))
   return "capture backing permissions";
  memcpy((uint8_t*)destination+done,v.bytes,(size_t)n);done+=n;
 }
 return NULL;
}
static const char *gpu_capture_first_texture(const GpuEngineState *engine) {
 const GpuGraphicsState *g=&engine->graphics;
 if(!g->valid[0x2608/4] || !g->valid[0x1234/4] || g->values[0x1234/4])
  return "texture handle binding unknown or unsupported";
 unsigned slot=g->values[0x2608/4];
 if(slot>=32 || !engine->constant_buffers[4][slot].known || !engine->constant_buffers[4][slot].enabled ||
    engine->constant_buffers[4][slot].size<36)return "texture handle cbuf unavailable";
 GpuAddressSpace *space=engine->memory_context;uint32_t handle,w[8];
 uint64_t cb=engine->constant_buffers[4][slot].address;
 if(cb>=(1ull<<40)-32)return "texture cbuf overflow";
 const char *error=gpu_capture_read(space,cb+32,4,&handle,0);if(error)return error;
 unsigned index=handle&0xfffff,base=0x1574/4;
 if(!g->valid[base] || !g->valid[base+1] || !g->valid[base+2] || g->values[base]>255 ||
    g->values[base+2]>0x3fffff || index>g->values[base+2])return "texture header pool unavailable";
 uint64_t pool=((uint64_t)g->values[base]<<32)|g->values[base+1];
 error=gpu_capture_read(space,pool+(uint64_t)index*32,32,w,0);if(error)return error;
 GpuTextureExtent t;error=gpu_texture_extent(w,&t);if(error)return error;
 printf("GPU first texture handle=0x%x image=%u address=0x%llx width=%u height=%u block-height=%u bytes=%llu\n",
  handle,index,(unsigned long long)t.address,t.width,t.height,t.block_height,(unsigned long long)t.size);
 uint8_t *bytes=malloc((size_t)t.size);if(!bytes)return "texture capture allocation";
 error=gpu_capture_read(space,t.address,t.size,bytes,1);
 if(error) {free(bytes);return error;}
 HANDLE file=CreateFileA("gpu-first-texture-z24s8.bin",GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
 if(file==INVALID_HANDLE_VALUE) {free(bytes);return "texture capture create failed";}
 DWORD written=0;BOOL ok=WriteFile(file,bytes,(DWORD)t.size,&written,NULL),closed=CloseHandle(file);free(bytes);
 if(!ok || !closed || written!=t.size)return "texture capture incomplete write";
 printf("GPU first texture captured=%lu complete=1\n",written);return NULL;
}
