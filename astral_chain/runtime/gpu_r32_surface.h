/* Canonical RF32 surface writeback. Caller must first validate draw state and
 * wait for GPU completion. This helper never authorizes or completes a draw. */
#pragma once
#include <stddef.h>
#include <string.h>
#include "gpu_graphics_state.h"
typedef struct {
 uint64_t address,extent;
 unsigned width,height,pitch,block_height;
} GpuR32Surface;
static const char *gpu_r32_layout(const GpuR32Surface *s) {
 if(!s || !s->width || !s->height || s->width>16384 || s->height>16384 || s->block_height>5)
  return "RF32 dimensions or tile height";
 uint64_t pitch=((uint64_t)s->width*4+63)&~63ull,rows=8ull<<s->block_height;
 uint64_t extent=((s->height+rows-1)&~(rows-1))*pitch;
 if(s->pitch!=pitch || s->extent!=extent || extent>64*1024*1024 || (s->address&511) ||
    s->address>=(1ull<<40) || extent>(1ull<<40)-s->address)return "RF32 extent or address";
 return NULL;
}
static const char *gpu_r32_description(const GpuGraphicsState *g,unsigned target,unsigned layer,GpuR32Surface *out) {
 if(!g || !out || target>=8)return "RF32 target unavailable";
 uint32_t f[8];for(unsigned i=0;i<8;i++) {
  const char *error=gpu_graphics_read(g,0x200+target*16+i,&f[i]);if(error)return error;
 }
 if(f[0]>255 || f[4]!=0xe5 || (f[5]&~0x70u) || ((f[5]>>4)&7)>5 ||
    !f[6] || f[6]>65535 || layer>=f[6])return "unsupported RF32 target format or layers";
 GpuR32Surface s={0};s.address=((uint64_t)f[0]<<32)|f[1];s.width=f[2];s.height=f[3];s.block_height=(f[5]>>4)&7;
 if(!s.width || !s.height || s.width>16384 || s.height>16384)return "RF32 dimensions";
 s.pitch=(s.width*4+63)&~63u;uint64_t rows=8ull<<s.block_height;
 s.extent=((s.height+rows-1)&~(rows-1))*s.pitch;
 uint64_t stride=(uint64_t)f[7]*4,offset=stride*layer;
 if((stride&511) || stride<s.extent || offset>(1ull<<40)-s.address)return "RF32 array pitch or layer overflow";
 s.address+=offset;const char *error=gpu_r32_layout(&s);if(error)return error;
 *out=s;return NULL;
}
static const char *gpu_r32_pack_check(const GpuR32Surface *s,void *destination,size_t capacity,
 const void *source,size_t size) {
 const char *error=gpu_r32_layout(s);if(error)return error;
 if(!source || !destination || size!=(uint64_t)s->width*s->height*4 || capacity<s->extent)
  return "RF32 source or destination extent";
 uintptr_t src=(uintptr_t)source,dst=(uintptr_t)destination;
 if((src>=dst && src-dst<s->extent) || (dst>=src && dst-src<size))return "RF32 source/destination alias";
 return NULL;
}
static const char *gpu_r32_pack(const GpuR32Surface *s,void *destination,size_t capacity,
 const void *source,size_t size) {
 const char *error=gpu_r32_pack_check(s,destination,capacity,source,size);if(error)return error;
 uint8_t *dst=destination;const uint8_t *src=source;uint64_t rows=8ull<<s->block_height;
 /* Checks precede all writes. Copy IEEE bits, including signed zero/NaNs;
  * padding and other array layers remain untouched. No color conversion. */
 for(unsigned y=0;y<s->height;y++)for(unsigned x=0;x<s->width;x++) {
  uint64_t xb=(uint64_t)x*4;
  uint64_t at=(y/rows)*rows*s->pitch+(xb/64)*512*(1ull<<s->block_height)+((y%rows)/8)*512;
  at+=((xb%64)/32)*256+((y%8)/2)*64+((xb%32)/16)*32+(y%2)*16+xb%16;
  memcpy(dst+at,src+((uint64_t)y*s->width+x)*4,4);
 }
 return NULL;
}
