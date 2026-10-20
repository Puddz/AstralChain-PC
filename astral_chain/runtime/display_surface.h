/* Canonical RGBA8 block-linear storage -> tightly packed top-down BGRA pixels.
 * Scanout uses the descriptor's block height unchanged (not mip heuristics). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "graphic_buffer.h"
static const char *display_surface_decode(const BqGraphicBuffer *b,const uint8_t *source,size_t size,
 unsigned left,unsigned top,unsigned right,unsigned bottom,uint8_t *output,size_t capacity) {
 if(!source || !output || b->format!=1 || !b->width || !b->height || b->width>16384 || b->height>16384 ||
    b->stride<b->width || b->stride>16384 || b->pitch!=b->stride*4 || (b->pitch&63) || b->block_height>5)
  return "display surface layout";
 if(left>=right || top>=bottom || right>b->width || bottom>b->height) return "display surface crop";
 uint64_t rows=8ull<<b->block_height,extent=((b->height+rows-1)&~(rows-1))*b->pitch;
 uint64_t pixels=(uint64_t)(right-left)*(bottom-top);
 if(extent>b->size || b->size>size || pixels>16777216 || pixels*4>capacity) return "display surface extent";
 for(unsigned y=top;y<bottom;y++) for(unsigned x=left;x<right;x++) {
  uint64_t xb=(uint64_t)x*4;
  uint64_t at=(y/rows)*rows*b->pitch+(xb/64)*512*(1ull<<b->block_height)+((y%rows)/8)*512;
  at+=((xb%64)/32)*256+((y%8)/2)*64+((xb%32)/16)*32+(y%2)*16+xb%16;
  uint64_t out=((uint64_t)(y-top)*(right-left)+x-left)*4;
  output[out]=source[at+2];output[out+1]=source[at+1];output[out+2]=source[at];output[out+3]=source[at+3];
 }
 return NULL;
}
