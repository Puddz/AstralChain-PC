/* Reviewed Switch single-plane RGBA graphic-buffer wire layout (libnx).
 * Retaining a buffer does not decode its block-linear pixels. */
#pragma once
#include <stdint.h>
#include <string.h>
typedef struct {
 uint8_t wire[364];
 unsigned object,width,height,stride,format,usage,offset,pitch,block_height,reference;
 uint64_t size;
} BqGraphicBuffer;
static uint32_t gb_u32(const uint8_t *p) {
 return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint64_t gb_u64(const uint8_t *p) {return gb_u32(p)|((uint64_t)gb_u32(p+4)<<32);}
static const char *gb_decode(const uint8_t *p,unsigned length,BqGraphicBuffer *result) {
 if(length!=364 || gb_u32(p)!=0x47424652 || gb_u32(p+32) || gb_u32(p+36)!=81)
  return "unsupported graphic buffer envelope";
 const uint8_t *n=p+40,*plane=n+52;
 if(gb_u32(n)!=0xffffffffu || gb_u32(n+12)!=0xdaffcaff || gb_u32(n+44)!=1 || gb_u32(n+8) || gb_u32(n+20) || gb_u32(n+48))
  return "unsupported native graphic buffer";
 BqGraphicBuffer b={0};
 b.object=gb_u32(n+4);b.width=gb_u32(p+4);b.height=gb_u32(p+8);b.stride=gb_u32(p+12);
 b.format=gb_u32(p+16);b.usage=gb_u32(p+20);b.offset=gb_u32(plane+28);
 b.pitch=gb_u32(plane+20);b.block_height=gb_u32(plane+36);b.size=gb_u64(plane+56);
 if(!b.object || !b.width || !b.height || b.width>16384 || b.height>16384 || b.stride<b.width || b.stride>16384 || b.format!=1)
  return "unsupported graphic buffer dimensions or format";
 if(gb_u32(n+24)!=b.usage || gb_u32(n+28)!=b.format || gb_u32(n+32)!=b.format || gb_u32(n+36)!=b.stride ||
    gb_u32(plane)!=b.width || gb_u32(plane+4)!=b.height || gb_u32(n+40)!=b.size)
  return "inconsistent graphic buffer metadata";
 if(gb_u64(plane+8)!=0x0100532120ull || gb_u32(plane+16)!=3 || gb_u32(plane+32)!=0xfe || b.block_height>5 ||
    b.pitch!=b.stride*4 || (b.pitch&63) || (b.offset&511) || gb_u32(plane+40) || gb_u32(plane+44) || gb_u64(plane+48))
  return "unsupported graphic buffer plane layout";
 uint64_t tile_rows=8ull<<b.block_height,rows=(b.height+tile_rows-1)&~(tile_rows-1);
 if(b.size<rows*b.pitch) return "graphic buffer plane extent";
 /* Compression fields and unused planes are not interpreted as plain RGBA. */
 for(unsigned i=64;i<88*3;i++) if(plane[i]) return "unsupported graphic buffer extra planes or compression";
 memcpy(b.wire,p,364);*result=b;return NULL;
}
