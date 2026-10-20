#pragma once
#include "gpu_graphics_state.h"
#include <math.h>
#include <string.h>
typedef struct {
 uint64_t address,extent;
 unsigned width,height,pitch,block_height,left,top,right,bottom,mask;
 uint8_t color[4];
 uint32_t packed_color,packed_mask;
 unsigned depth_format;
 unsigned depth_stencil; /* Shared packed 32-bit surface writer. */
} GpuColorClear;
/* Finite float -> clamped UNORM8, nearest with ties to even. Integer arithmetic
 * keeps this independent of host rounding modes and avoids float casts of NaN. */
static const char *gpu_clear_unorm8(uint32_t bits,uint8_t *result) {
 unsigned exponent=(bits>>23)&255;
 if(exponent==255) return "nonfinite clear color unsupported";
 if(bits>>31 || exponent<118) {*result=0;return NULL;}
 if(bits>=0x3f800000u) {*result=255;return NULL;}
 unsigned shift=150-exponent;
 uint64_t product=(uint64_t)((bits&0x7fffff)|0x800000)*255;
 uint64_t whole=product>>shift,remainder=product&((1ull<<shift)-1),half=1ull<<(shift-1);
 *result=(uint8_t)(whole+(remainder>half || (remainder==half && (whole&1))));return NULL;
}
/* IEC sRGB transfer on RGB only; alpha remains linear UNORM8. */
static const char *gpu_clear_srgb8(uint32_t bits,uint8_t *result) {
 unsigned exponent=(bits>>23)&255;
 if(exponent==255) return "nonfinite sRGB clear unsupported";
 if(bits>>31) {*result=0;return NULL;}
 if(bits>=0x3f800000u) {*result=255;return NULL;}
 float f;memcpy(&f,&bits,sizeof(f));double linear=f;
 double encoded=linear<=0.0031308 ? 12.92*linear : 1.055*pow(linear,1.0/2.4)-0.055;
 double scaled=encoded*255;unsigned whole=(unsigned)scaled;double remainder=scaled-whole;
 *result=(uint8_t)(whole+(remainder>0.5 || (remainder==0.5 && (whole&1))));return NULL;
}
/* Unsigned 5-bit-exponent float with 5 or 6 mantissa bits. Finite overflow
 * saturates, matching DirectXMath's packed R11G11B10 conversion policy. */
static const char *gpu_clear_ufloat(uint32_t bits,unsigned mantissa,uint32_t *result) {
 unsigned exponent=(bits>>23)&255;
 if(mantissa!=5 && mantissa!=6) return "invalid packed float width";
 if(exponent==255) return "nonfinite packed clear color unsupported";
 if(bits>>31 || !exponent) {*result=0;return NULL;}
 unsigned shift=exponent<113 ? 136-mantissa-exponent : 23-mantissa;
 if(shift>32) {*result=0;return NULL;}
 uint64_t value=(bits&0x7fffff)|0x800000,half=1ull<<(shift-1);
 uint32_t rounded=(uint32_t)((value+half-1+((value>>shift)&1))>>shift);
 if(exponent<113) {*result=rounded;return NULL;}
 unsigned biased=exponent-112;
 if(rounded==(1u<<(mantissa+1))) {rounded>>=1;biased++;}
 *result=biased>=31 ? (31u<<mantissa)-1 : (biased<<mantissa)|(rounded&((1u<<mantissa)-1));
 return NULL;
}
static void gpu_clear_intersect(GpuColorClear *clear,unsigned x0,unsigned y0,unsigned x1,unsigned y1) {
 if(clear->left<x0) clear->left=x0;if(clear->top<y0) clear->top=y0;
 if(clear->right>x1) clear->right=x1;if(clear->bottom>y1) clear->bottom=y1;
}
static const char *gpu_clear_clip(const GpuGraphicsState *g,uint32_t control,GpuColorClear *c) {
 /* Surface clip is always active. Additional rectangles intersect it. */
 const unsigned masks[]={0,0x10,0x100,0x1000},horizontal[]={0xff4/4,0xd6c/4,0xe04/4,0xc00/4};
 for(unsigned i=0;i<4;i++) {
  if(i && !(control&masks[i])) continue;
  if(i==2) {uint32_t enabled;const char *error=gpu_graphics_read(g,0xe00/4,&enabled);if(error) return error;if(!enabled) continue;}
  uint32_t x,y;const char *error=gpu_graphics_read(g,horizontal[i],&x);if(error) return error;
  error=gpu_graphics_read(g,horizontal[i]+1,&y);if(error) return error;
  unsigned x0=x&65535,y0=y&65535,x1=x>>16,y1=y>>16;
  if(i==0 || i==3) {x1+=x0;y1+=y0;}
  gpu_clear_intersect(c,x0,y0,x1,y1);
 }
 return NULL;
}
static const char *gpu_color_clear_description(const GpuGraphicsState *g,uint32_t command,GpuColorClear *out) {
 if(command&~0x03fffffcu || !(command&0x3c) || ((command>>6)&15)>=8) return "unsupported color clear layer, target or depth/stencil flags";
 unsigned target=(command>>6)&15,layer=(command>>10)&65535;uint32_t fields[8],control,samples,origin;
 for(unsigned i=0;i<8;i++) {const char *error=gpu_graphics_read(g,0x200+target*16+i,&fields[i]);if(error) return error;}
 const unsigned methods[]={0x10f8/4,0x15d0/4,0x13ac/4};uint32_t *values[]={&control,&samples,&origin};
 for(unsigned i=0;i<3;i++) {const char *error=gpu_graphics_read(g,methods[i],values[i]);if(error) return error;}
 if(samples || (origin&~0x10u) || (fields[4]!=0xd5 && fields[4]!=0xd6 && fields[4]!=0xe0 && fields[4]!=0xe5) || (!fields[6] || layer>=fields[6]) || (fields[5]&~0x70u) || ((fields[5]>>4)&7)>5)
  return "unsupported color clear surface layout";
 GpuColorClear c={0};c.address=((uint64_t)fields[0]<<32)|fields[1];c.width=fields[2];c.height=fields[3];
 if(!c.width || !c.height || c.width>16384 || c.height>16384 || (uint64_t)c.width*c.height>16777216) return "color clear dimensions";
 c.block_height=(fields[5]>>4)&7;c.pitch=(c.width*4+63)&~63u;
 uint64_t rows=8ull<<c.block_height;c.extent=((c.height+rows-1)&~(rows-1))*c.pitch;
 uint64_t stride=(uint64_t)fields[7]*4;
 if((c.address&511) || (stride&511) || stride<c.extent || c.extent>0x10000000000ull-c.address) return "color clear surface extent";
 /* CLEAR_SURFACE selects one array slice; SET_RT_LAYER is draw state.
  * The array pitch is in words, independently padded from the slice extent. */
 uint64_t layer_offset=stride*layer;
 if(layer_offset>0x10000000000ull-c.address || c.extent>0x10000000000ull-c.address-layer_offset)
  return "color clear layer address overflow";
 c.address+=layer_offset;
 c.right=c.width;c.bottom=c.height;c.mask=(command>>2)&15;
 for(unsigned i=0;i<4;i++) if(c.mask&(1u<<i)) {
  if(fields[4]==0xe5 && i!=0)continue; /* RF32 contains only red. */
  if(fields[4]==0xe0 && i==3) continue; /* Packed HDR has no alpha channel. */
  uint32_t bits;const char *error=gpu_graphics_read(g,0xd80/4+i,&bits);if(error) return error;
  if(fields[4]==0xe5) {
   if(((bits>>23)&255)==255)return "nonfinite RF32 clear unsupported";
   c.packed_color=bits;c.packed_mask=0xffffffffu;
  } else if(fields[4]==0xd5 || fields[4]==0xd6) {
   error=gpu_clear_unorm8(bits,&c.color[i]);if(error) return error;
   if(fields[4]==0xd6 && i<3) {
    uint8_t encoded;error=gpu_clear_srgb8(bits,&encoded);if(error) return error;
    /* No reset-state guess: execute unknown enable only when both results agree. */
    if(!g->valid[0x15b8/4] && encoded!=c.color[i]) return "sRGB write conversion state unknown";
    if(g->valid[0x15b8/4] && g->values[0x15b8/4]) c.color[i]=encoded;
   }
   c.packed_color|=(uint32_t)c.color[i]<<(i*8);c.packed_mask|=255u<<(i*8);
  } else {
   uint32_t packed;unsigned shift=i==2 ? 22 : i*11,mantissa=i==2 ? 5 : 6;
   error=gpu_clear_ufloat(bits,mantissa,&packed);if(error) return error;
   c.packed_color|=packed<<shift;c.packed_mask|=((1u<<(mantissa+5))-1)<<shift;
  }
 }
 const char *error=gpu_clear_clip(g,control,&c);if(error) return error;
 *out=c;return NULL;
}
/* NVIDIA Z24S8 (0x14) maps to PIPE_FORMAT_S8_UINT_Z24_UNORM:
 * stencil occupies bits 7:0, depth bits 31:8. See Mesa nil_formats.csv. */
static const char *gpu_depth_clear_description(const GpuGraphicsState *g,uint32_t command,GpuColorClear *out) {
 if(!(command&3) || (command&~0x03fffc03u)) return "unsupported combined depth clear";
 unsigned layer=(command>>10)&65535;
 const unsigned methods[]={0xfe0,0xfe4,0xfe8,0xfec,0xff0,0x1228,0x122c,0x1230,0x1538,0x15d0,0x13ac,0x10f8};
 uint32_t f[12];
 for(unsigned i=0;i<12;i++) {const char *error=gpu_graphics_read(g,methods[i]/4,&f[i]);if(error) return error;}
 if((f[2]!=0x14 && f[2]!=0x0a) || (f[3]&~0x70u) || ((f[3]>>4)&7)>5 || f[8]!=1 || f[9] || (f[10]&~0x10u)) return "unsupported depth clear surface layout";
 unsigned layers=(f[7]&0x10000)?1:(f[7]&65535);
 if(!layers || layer>=layers)return "depth clear layer outside surface";
 GpuColorClear c={0};c.depth_format=f[2];c.depth_stencil=1;c.address=((uint64_t)f[0]<<32)|f[1];c.width=f[5];c.height=f[6];
 if(!c.width || !c.height || c.width>16384 || c.height>16384 || (uint64_t)c.width*c.height>16777216) return "depth clear dimensions";
 c.block_height=(f[3]>>4)&7;c.pitch=(c.width*4+63)&~63u;
 uint64_t rows=8ull<<c.block_height;c.extent=((c.height+rows-1)&~(rows-1))*c.pitch;
 uint64_t stride=(uint64_t)f[4]*4;
 if((c.address&511) || (stride&511) || stride<c.extent || c.extent>0x10000000000ull-c.address) return "depth clear surface extent";
 uint64_t layer_offset=stride*layer;
 if(layer_offset>0x10000000000ull-c.address || c.extent>0x10000000000ull-c.address-layer_offset)
  return "depth clear layer address overflow";
 c.address+=layer_offset;
 c.right=c.width;c.bottom=c.height;
 if(command&1) {
  uint32_t bits;const char *error=gpu_graphics_read(g,0xd90/4,&bits);if(error) return error;
  unsigned exponent=(bits>>23)&255;uint32_t depth;
  if(exponent==255) return "nonfinite depth clear unsupported";
  if(f[2]==0x0a) {
   /* Preserve finite normalized float depths exactly; out-of-range clamping
    * semantics are not assumed for floating depth storage. */
   if(bits!=0x80000000u && ((bits>>31) || bits>0x3f800000u))return "floating depth clear range unsupported";
   c.packed_color=bits;c.packed_mask=0xffffffffu;
  } else {
  if((bits>>31) || exponent<102) depth=0;
  else if(bits>=0x3f800000u) depth=0xffffff;
  else {
   /* Finite clamped UNORM24, round nearest even; independent of host FP mode. */
   unsigned shift=150-exponent;uint64_t product=(uint64_t)((bits&0x7fffff)|0x800000)*0xffffff;
   uint64_t whole=product>>shift,remainder=product&((1ull<<shift)-1),half=1ull<<(shift-1);
   depth=(uint32_t)(whole+(remainder>half || (remainder==half && (whole&1))));
  }
  c.packed_color=depth<<8;c.packed_mask=0xffffff00u;
  }
 }
 if((command&2) && f[2]==0x14) {
  uint32_t stencil,mask=255;const char *error=gpu_graphics_read(g,0xda0/4,&stencil);if(error) return error;
  if(f[11]&1) {error=gpu_graphics_read(g,0x139c/4,&mask);if(error) return error;}
  c.packed_color|=stencil;c.packed_mask|=mask;
 }
 const char *error=gpu_clear_clip(g,f[11],&c);if(error) return error;
 *out=c;return NULL;
}
