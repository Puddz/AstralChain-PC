/* Retained Maxwell 3D configuration, indexed by method word address.
 * Only reviewed configuration methods belong here: never command triggers.
 * Field layouts: NVIDIA open-gpu-doc classes/3d/clb197.h; CSAA enable:
 * devkitPro/deko3d source/maxwell/engine_3d.def (method 0x56d). */
#pragma once
#include <stdint.h>
enum { GPU_GRAPHICS_METHODS=0xe00 };
typedef struct {
 uint32_t values[GPU_GRAPHICS_METHODS];
 uint8_t valid[GPU_GRAPHICS_METHODS],dirty[GPU_GRAPHICS_METHODS];
 uint64_t zcull_invalidation_serial;
} GpuGraphicsState;
static int gpu_blend_operation_valid(uint32_t value) {
 return (value>=1 && value<=5) || (value>=0x8006 && value<=0x8008) || value==0x800a || value==0x800b;
}
static int gpu_stencil_operation_valid(uint32_t value) {
 return value<=8 || (value>=0x1e00 && value<=0x1e03) || value==0x150a || value==0x8507 || value==0x8508;
}
static int gpu_blend_coefficient_valid(uint32_t value,unsigned source) {
 if(value>=1 && value<=19)return source || (value!=12 && value!=13);
 return value==0x4000 || value==0x4001 || (value>=0x4300 && value<=0x4308) ||
        (value>=0xc001 && value<=0xc004) || (value>=0xc900 && value<=0xc903);
}
static int gpu_vertex_width_valid(unsigned value) {
 switch(value) {
  case 1:case 2:case 3:case 4:case 5:case 10:case 15:case 18:case 19:
  case 24:case 27:case 29:case 47:case 48:case 49:case 50:case 51:case 52:return 1;
  default:return 0;
 }
}
static int gpu_graphics_mask(unsigned method,uint32_t *mask) {
 if(method>=0x11e0/4 && method<0x11f0/4) {*mask=0xffffffffu;return 1;} /* 16 sample XY positions, four bits per coordinate */
 if(method>=0x1360/4 && method<0x1380/4) {*mask=1;return 1;} /* eight blend enables */
 if(method>=0x1a00/4 && method<0x1a20/4) {*mask=0x1111;return 1;} /* per-target RGBA write enables */
 if(method>=0x1160/4 && method<0x11e0/4) {*mask=0xbfffffdfu;return 1;} /* attribute banks A/B */
 if(method>=0x1c00/4 && method<0x1e00/4) {
  const uint32_t masks[]={0x1fff,255,0xffffffffu,0xffffffffu};
  *mask=masks[method&3];return 1; /* 32 stream stride/enables, addresses and frequencies */
 }
 if(method>=0x1f00/4 && method<0x2000/4) {*mask=(method&1)?0xffffffffu:255;return 1;} /* 32 limits */
 if(method>=0x2000/4 && method<0x2180/4 && (method&15)<5) {
  const uint32_t masks[]={0xf1,0xffffffffu,1,255,7};
  *mask=masks[method&15];return 1; /* six pipeline slots: enable/type, program, A, registers, group */
 }
 if(method>=0x1e00/4 && method<0x1f00/4 && (method&7)<7) {
  *mask=(method&7)?0xffffffffu:1;return 1; /* 8 separate RGB/alpha blend descriptors */
 }
 if(method>=0x200 && method<0x280 && (method&15)<8) {
  const uint32_t masks[]={255,0xffffffffu,0x0fffffffu,0x1ffff,255,0x11fff,0x0fffffffu,0xffffffffu};
  *mask=masks[method&15];return 1; /* eight color-target descriptors, no rendering */
 }
 if(method>=0x280 && method<0x300 && (method&7)<6) {*mask=0xffffffffu;return 1;} /* 16 viewport XYZ scales/offsets */
 if(method>=0x300 && method<0x340) {*mask=0xffffffffu;return 1;} /* viewport clip XY/width/height and raw float min/max Z */
 if(method>=0xd00 && method<0xe00) {*mask=0xffffffffu;return 1;} /* MME scratch[256] */
 if(method>=0x340 && method<0x350) {*mask=0xffffffffu;return 1;} /* 8 clip rectangles */
 if(method>=0x380 && method<0x3c0 && !(method&3)) {*mask=1;return 1;} /* 16 scissor enables */
 if(method>=0x380 && method<0x3c0 && ((method&3)==1 || (method&3)==2)) {*mask=0xffffffffu;return 1;} /* scissor XY min/max */
 if(method>=0x620 && method<0x630) {*mask=1;return 1;} /* 16 vertex-stream instance enables */
 switch(method*4) {
  case 0x0f1c:*mask=1;return 1; /* post-depth pixel shader input mask */
  case 0x0360:*mask=0xffffffffu;return 1; /* four subtile resource fractions */
  case 0x0364:*mask=255;return 1; /* primitive subtile fraction */
  case 0x0754:*mask=63;return 1; /* hybrid AA pass count / centroid / extended count */
  case 0x1224:*mask=15;return 1; /* pixel shader interlock mode, tile size and ordering */
  case 0x0210:*mask=1;return 1; /* API-mandated early depth testing, consumed by draw backend */
  case 0x0f40:*mask=1;return 1; /* constant-color rendering enable */
  case 0x0f44:case 0x0f48:case 0x0f4c:case 0x0f50:
   *mask=0xffffffffu;return 1; /* raw constant rendering RGBA, separate from clear/blend color */
  case 0x12e0:*mask=15;return 1; /* alpha-to-coverage dither footprint enum */
  case 0x12e8:*mask=1;return 1; /* depth write enable */
  case 0x130c:case 0x1314:case 0x1390:case 0x15a4:
   *mask=0xffffffffu;return 1; /* depth / alpha / front-back stencil comparison enums */
  case 0x1384:case 0x1388:case 0x138c:case 0x1598:case 0x159c:case 0x15a0:
   *mask=0xffffffffu;return 1; /* front-back stencil fail / depth fail / pass operations */
  case 0x19c4:*mask=1;return 1; /* logic operation enable */
  case 0x19c8:*mask=0xffffffffu;return 1; /* logic operation enum */
  case 0x07e8:case 0x07f0:*mask=255;return 1; /* ZCULL storage start/limit upper */
  case 0x07ec:case 0x07f4:*mask=0xffffffffu;return 1; /* ZCULL storage start/limit lower */
  case 0x0dc8:case 0x12cc:case 0x12ec:case 0x1380:case 0x1918:
   *mask=1;return 1; /* polygon offset, depth/alpha/stencil tests, face culling */
  case 0x0dd0:*mask=3;return 1; /* iterated blend enable and alpha enable */
  case 0x0f54:case 0x1394:*mask=255;return 1; /* back/front stencil reference */
  case 0x1310:case 0x131c:case 0x1320:case 0x1324:case 0x1328:
  case 0x156c:case 0x15bc:case 0x187c:
   *mask=0xffffffffu;return 1; /* raw float alpha reference, blend color, depth bias */
  case 0x0dac:case 0x0db0:case 0x191c:case 0x1920:
   *mask=0xffffffffu;return 1; /* polygon/front/cull enums validated below */
  case 0x15b8:*mask=1;return 1; /* sRGB write conversion enable */
  case 0x0dbc:*mask=0x30001;return 1; /* ZCULL comparison direction and depth representation */
  case 0x0d90:*mask=0xffffffffu;return 1; /* raw float depth clear value */
  case 0x0da0:*mask=255;return 1; /* stencil clear value */
  case 0x0fe0:*mask=255;return 1; /* depth target address upper */
  case 0x0fe4:case 0x0ff0:case 0x07e0:*mask=0xffffffffu;return 1; /* address lower / array pitch / ZCULL location */
  case 0x0fe8:*mask=31;return 1; /* depth target format, checked below */
  case 0x0fec:*mask=0xfff;return 1; /* depth target block dimensions */
  case 0x1228:*mask=0x0fffffff;return 1; /* depth target width */
  case 0x122c:*mask=0x1ffff;return 1; /* depth target height */
  case 0x07c0:case 0x07c4:case 0x07c8:case 0x07cc:case 0x07e4:case 0x15fc:case 0x1600:
   *mask=0xffff;return 1; /* ZCULL dimensions, offsets and per-layer aliquots */
  case 0x15c8:*mask=15;return 1; /* ZCULL region format */
  case 0x02e8:*mask=0x0ffffff1;return 1; /* ZCULL subregion enable and normalized aliquots */
  case 0x0d6c:case 0x0d70:*mask=0xffffffffu;return 1; /* optional clear rectangle */
  case 0x0d74:*mask=0xffffffffu;return 1; /* deko3d DrawArraysFirst: non-indexed first vertex */
  case 0x0d80:case 0x0d84:case 0x0d88:case 0x0d8c:*mask=0xffffffffu;return 1; /* raw clear-color components; not the clear trigger */
  case 0x121c:*mask=0x0fffffff;return 1; /* target count and eight 3-bit selectors */
  case 0x1538:*mask=1;return 1; /* depth-target count */
  case 0x0ff4:case 0x0ff8:case 0x13b0:case 0x13b4:*mask=0xffffffffu;return 1; /* surface clip / raw float line widths */
  case 0x0790:case 0x155c:case 0x1574:*mask=255;return 1; /* local memory / sampler / header address upper */
  case 0x0794:case 0x079c:case 0x1560:case 0x1578:*mask=0xffffffffu;return 1; /* address / size lower */
  case 0x0798:*mask=63;return 1; /* shader local memory size upper */
  case 0x1564:*mask=0xfffff;return 1; /* sampler maximum index */
  case 0x157c:*mask=0x3fffff;return 1; /* texture-header maximum index */
  case 0x07a4:case 0x07a8:*mask=0x7fff;return 1; /* color/depth ZBC slot-disable masks */
  case 0x0f60:case 0x1134:*mask=1;return 1; /* tile cache enable / mutable-method policy */
  case 0x0f64:*mask=0xffffffffu;return 1; /* tile width/height, 16 bits each */
  case 0x0f68:case 0x0f6c:case 0x0f70:case 0x1108:
   *mask=0xffffffffu;return 1; /* opaque tile configuration; initialization contract checked below */
  case 0x17c8:case 0x17d0:case 0x1608:*mask=255;return 1; /* index start/limit and program address upper */
  case 0x17cc:case 0x17d4:case 0x17dc:case 0x160c:*mask=0xffffffffu;return 1; /* address lower / first index */
  case 0x17d8:*mask=3;return 1; /* index element size, checked below */
  case 0x1514:case 0x0d9c:case 0x1520:case 0x1148:case 0x0fd8:*mask=1;return 1;
  case 0x1510:case 0x1398:case 0x139c:case 0x0f58:case 0x0f5c:*mask=255;return 1; /* clip planes / stencil masks */
  case 0x0fbc:case 0x0fc0:case 0x0fc4:case 0x0fc8:*mask=65535;return 1; /* sample masks */
  case 0x1604:*mask=0x1fff;return 1; /* point sprite R mode, origin, generated coordinates */
  case 0x1910:*mask=0xff1;return 1; /* shader point size enable / attribute slot */
  case 0x1518:*mask=0xffffffffu;return 1; /* point size, raw float */
  case 0x1230:*mask=0x1ffff;return 1; /* depth-target third dimension / array control */
  case 0x02ec:*mask=0xff1;return 1; /* raster bounding box mode / pad */
  case 0x0fb4:*mask=1;return 1; /* only two defined TIR modes */
  case 0x11f8:*mask=0x71;return 1; /* coverage-to-color enable / target */
  case 0x16b4:*mask=3;return 1; /* alpha-to-coverage qualifiers */
  case 0x12e4:case 0x15e4:case 0x192c:case 0x0d7c:*mask=1;return 1; /* per-target blending, edge, viewport enables */
  case 0x13ac:*mask=0x11;return 1; /* window origin / Y flip */
  case 0x193c:*mask=0x3c9f;return 1; /* viewport clip/guardband modes */
  case 0x2380:*mask=0x1ffff;return 1; /* constant-buffer selector byte size */
  case 0x2384:case 0x0f84:*mask=255;return 1; /* selector / substitute address upper */
  case 0x2388:case 0x0f88:*mask=0xffffffffu;return 1; /* address lower */
  case 0x238c:*mask=0xffff;return 1; /* constant-buffer upload byte offset */
  case 0x1968:case 0x196c:*mask=0x11;return 1; /* ZCULL enables / unbounded Z limits */
  case 0x1590:*mask=0x3f;return 1; /* active ZCULL region */
  case 0x0dd8:*mask=0xffff0307u;return 1; /* ZCULL stencil comparison, flags, reference/mask */
  case 0x1534:case 0x15b4:*mask=1;return 1; /* antialias / CSAA enable */
  case 0x15d0:*mask=15;return 1; /* antialias sample mode */
  case 0x153c:*mask=0x11;return 1; /* alpha-to-coverage / alpha-to-one */
  case 0x10e8:*mask=0x003f003f;return 1; /* FP11 color reduction thresholds: two six-bit fields */
  case 0x10cc:case 0x10e0:case 0x10e4:case 0x10ec:case 0x10f0:
   *mask=0x00ff00ff;return 1; /* color reduction thresholds */
  case 0x074c:*mask=255;return 1; /* alpha fraction */
  case 0x10f8:*mask=0x1111;return 1; /* clear rectangle/mask policy, no clear triggered */
  case 0x0de8:case 0x1234:case 0x1684:case 0x1528: /* restart, sampler binding, provoking vertex, exceptions */
  case 0x151c:case 0x020c:case 0x030c: /* ZCULL stats, line width, conditional constants */
  case 0x0fdc:case 0x19c0:case 0x0f8c:case 0x1594: /* blend policies, polygon clip, two-sided stencil */
   *mask=1;return 1;
  case 0x165c:*mask=1;return 1; /* point center: only the two defined modes */
  case 0x1000:*mask=0x31;return 1; /* VAF cache policy and volatility */
  case 0x0218:case 0x10fc:case 0x1290:case 0x12d8:case 0x12dc:
   *mask=0x30;return 1; /* ROP cache policies, not cache-operation triggers */
  case 0x0d64:case 0x1220:*mask=15;return 1; /* call limit / compression threshold */
  case 0x1140:*mask=0x10;return 1; /* per-format blend enable */
  case 0x1610:*mask=0x3f;return 1; /* default attributes */
  case 0x164c:*mask=0x1000;return 1; /* vertex ID uses array start */
  case 0x0300:*mask=3;return 1; /* shader sample mask usage */
  case 0x2608:*mask=31;return 1; /* bindless texture constant-buffer slot */
  case 0x0310:*mask=0xffff;return 1; /* shader ISA version; checked below */
  case 0x077c:*mask=0xffffffffu;return 1; /* local memory window base */
  case 0x1130:*mask=0x13;return 1; /* target-independent rasterization policy */
  case 0x0374:*mask=0x11;return 1; /* workload distribution */
  case 0x15cc:*mask=0x1ffff;return 1; /* render target layer and control */
  case 0x1450:*mask=0xffffffffu;return 1; /* pixel-shader warp watermarks */
  default:return 0;
 }
}
static const char *gpu_graphics_write(GpuGraphicsState *graphics,unsigned method,uint32_t value) {
 uint32_t mask;
 if(method>=GPU_GRAPHICS_METHODS || !gpu_graphics_mask(method,&mask)) return "unsupported graphics state method";
 if(value&~mask) return "unsupported graphics state bits";
 if(method==0x1224/4 && (value&3)==3)return "unsupported pixel shader interlock mode";
 if(method==0x12e0/4 && value>2)return "unsupported alpha-to-coverage dither footprint";
 if((method==0x130c/4 || method==0x1314/4 || method==0x1390/4 || method==0x15a4/4) &&
    !((value>=1 && value<=8) || (value>=0x200 && value<=0x207)))return "unsupported comparison function";
 if(((method>=0x1384/4 && method<=0x138c/4) || (method>=0x1598/4 && method<=0x15a0/4)) &&
    !gpu_stencil_operation_valid(value))return "unsupported stencil operation";
 if(method==0x19c8/4 && (value<0x1500 || value>0x150f))return "unsupported logic operation";
 if(method>=0x1160/4 && method<0x11e0/4 && !(value&64)) {
  if(!gpu_vertex_width_valid((value>>21)&63) || !((value>>27)&7))return "unsupported active vertex format";
 }
 if(method>=0x2000/4 && method<0x2180/4 && !(method&15) && ((value>>4)&15)>5)
  return "unsupported pipeline shader type";
 if(method>=0x1e00/4 && method<0x1f00/4) {
  unsigned field=method&7;
  if((field==1 || field==4) && !gpu_blend_operation_valid(value))return "unsupported blend operation";
  if((field==2 || field==3 || field==5 || field==6) && !gpu_blend_coefficient_valid(value,field==2 || field==5))
   return "unsupported blend coefficient";
 }
 if((method==0xdac/4 || method==0xdb0/4) && (value<0x1b00 || value>0x1b02)) return "unsupported polygon mode";
 if(method==0x191c/4 && value!=0x900 && value!=0x901) return "unsupported front face";
 if(method==0x1920/4 && value!=0x404 && value!=0x405 && value!=0x408) return "unsupported cull face";
 if(method==0x121c/4 && (value&15)>8) return "invalid color target count";
 if(method>=0x200 && method<0x280) {
  if((method&15)==4 && value!=0 && value!=0xd5 && value!=0xd6 && value!=0xe0 && value!=0xe5) return "unsupported color target format";
  if((method&15)==5 && ((value&15) || ((value>>4)&15)>5 || ((value>>8)&15)>5))
   return "unsupported color target tile dimensions";
 }
 if(method==0xfe8/4 && value!=0x14 && value!=0x0a) return "unsupported depth target format";
 if(method==0xfec/4 && ((value&15) || ((value>>4)&15)>5 || (value&0xf00))) return "unsupported depth target tile dimensions";
 if(method==0x15c8/4 && value>12) return "unsupported ZCULL region format";
 if((method==0x3da && value!=0x1109) || (method==0x3db && value!=0x08080202) ||
    (method==0x3dc && value!=0x80001) || (method==0x442 && value!=0x1f))
  return "unsupported tile configuration value";
 if(method==0x574 && (value==7 || value>11)) return "unsupported antialias sample mode";
 if(method==0xc4 && value!=0x503) return "unsupported shader ISA version";
 if(method==0x64f && (((value>>1)&3)==3 || ((value>>11)&7)==7)) return "unsupported viewport clip mode";
 if(method==0x5f6 && value==3) return "unsupported index element size";
 if(method==0x581 && (value&3)==3) return "unsupported point sprite R mode";
 /* NO_INVALIDATE clear requests cache invalidation, including repeated writes.
  * No ZCULL cache exists yet. A backend must consume this serial as well as
  * configuration dirty bits before using cached culling results. */
 if(method==0x376 && !(value&256)) {
  if(graphics->zcull_invalidation_serial==UINT64_MAX) return "ZCULL invalidation serial exhausted";
  graphics->zcull_invalidation_serial++;
 }
 if(!graphics->valid[method] || graphics->values[method]!=value) graphics->dirty[method]=1;
 graphics->values[method]=value;graphics->valid[method]=1;return NULL;
}
/* Unwritten state is unknown, not an invented hardware reset value. */
static const char *gpu_graphics_read(const GpuGraphicsState *graphics,unsigned method,uint32_t *value) {
 if(method>=GPU_GRAPHICS_METHODS || !graphics->valid[method]) return "uninitialized graphics state";
 *value=graphics->values[method];return NULL;
}
