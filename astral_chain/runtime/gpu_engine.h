/* Software engine state. Unsupported methods must never be treated as executed.
 * RAM limits are explicit implementation bounds, not verified hardware limits. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include "gpu_graphics_state.h"
#include "gpu_color_clear.h"
#include "gpu_compute_state.h"
#include "gpu_macro.h"
enum { GPU_MME_WORDS=4096, GPU_MME_ENTRIES=128, GPU_INSTRUMENTATION_RECORDS=256 };
/* Writer must validate the entire access before changing memory. */
typedef const char *(*GpuWriteWord)(void *,uint64_t,uint32_t);
typedef const char *(*GpuCheckWrite)(void *,uint64_t,uint64_t);
typedef const char *(*GpuIncrementSyncpoint)(void *,unsigned);
typedef struct {const char *error;unsigned completion_unknown;} GpuDrawResult;
typedef struct GpuEngineState {
 unsigned classes[8];
 GpuGraphicsState graphics;
 GpuComputeState compute;
 uint64_t host_l2_flushes,host_l2_invalidations;
 uint32_t host_reference;unsigned host_reference_valid;
 uint64_t host_reference_writes;
 /* No GPU caches exist yet. Future backends must consume these generations
  * before reusing shader/texture/sampler/header data or completing a flush. */
 uint64_t cache_operations[6];
 uint64_t pixel_shader_barriers[2]; /* indexed by SYSMEMBAR enable */
 uint64_t texture_wait_invalidations;
 uint32_t shadow_values[GPU_GRAPHICS_METHODS];
 uint8_t shadow_known[GPU_GRAPHICS_METHODS];
 unsigned shadow_mode,shadow_mode_known;
 GpuWriteWord write_word;
 GpuCheckWrite check_write;
 GpuCheckWrite decompress_surface;
 const char *(*clear_color)(void *,const GpuColorClear *);
 uint64_t color_clears,depth_stencil_clears;
 uint64_t surface_decompressions;
 void *memory_context;
 GpuIncrementSyncpoint increment_syncpoint;
 void *syncpoint_context;
 uint32_t report_semaphore[3];
 unsigned report_semaphore_valid;
 uint64_t report_semaphore_releases;
 uint64_t constant_buffer_words;
 struct {uint64_t address;uint32_t size;unsigned known,enabled,dirty;} constant_buffers[5][32];
 struct {
  uint32_t length,lines,upper,lower;
  unsigned valid,active;
  uint64_t address,remaining,words,completed;
 } inline_dma;
 /* Ordered native diagnostic stream, not emulation of hardware profiling.
  * Preserve repeats and method identity; never silently discard on overflow. */
 struct {uint32_t method,value;} instrumentation[GPU_INSTRUMENTATION_RECORDS];
 unsigned instrumentation_count;
 /* ESETUP context configuration. Unknown reset bits are not valid state.
  * Bit 0 is retained but its rendering meaning is not established. */
 struct GpuMaskedConfig {uint32_t value,known,dirty;} setup_config,tex_config04,tex_config08,sm_config178;
 /* Hardware watchdog configuration; host instruction budget is independent.
  * No hardware clock/timer emulation is implied by retaining this threshold. */
 struct GpuMaskedConfig macro_watchdog;
 /* Maxwell context initialization table plus deko3d's GM20B writes establish
  * retained GPC state here. Individual tile-cache field meanings are unknown. */
 struct GpuMaskedConfig tile_config[8];
 struct {uint32_t operation,clip_enable,corral_size;unsigned valid,dirty;} twod;
 uint32_t sph_version_check,aam_version_check;
 unsigned sph_checked,aam_checked;
 /* Backend operations must remain counted until their effects complete.
  * Inline DMA remains pending while its data stream is incomplete. */
 /* Native conservative policy: no hierarchical depth rejection. A future
  * renderer must still perform ordinary per-fragment depth/stencil tests. */
 unsigned conservative_zcull;
 uint64_t zcull_stores,zcull_loads,zcull_storage_start,zcull_storage_limit;
 unsigned pending_work;
 /* Normal first-instance BEGIN/END bookkeeping. No vertices are submitted by
  * BEGIN; draw-count commands require a real backend and still reject. */
 struct {unsigned active,topology;uint32_t instance_id,primitive_id;} draw;
 /* Success means completed, including guest writeback. Failure must leave
  * guest destinations unchanged. Unknown completion retains pending work. */
 GpuDrawResult (*indexed_draw)(const struct GpuEngineState *,uint32_t);
 uint64_t indexed_draws;
 void (*indexed_draw_failure_report)(void *,const GpuGraphicsState *,uint32_t);
 /* Non-indexed draw: first vertex from retained 0x0d74, count from 0x0d78. */
 GpuDrawResult (*vertex_draw)(const struct GpuEngineState *,uint32_t first,uint32_t count);
 uint64_t vertex_draws;
 uint32_t code[GPU_MME_WORDS],starts[GPU_MME_ENTRIES];
 uint8_t code_valid[GPU_MME_WORDS],start_valid[GPU_MME_ENTRIES];
 unsigned code_pointer,start_pointer,code_writes,start_writes;
 GpuMacro macro;
 unsigned macro_active,macro_slot,macro_subchannel;
 const char *macro_error;
 void (*macro_failure_report)(unsigned method,uint32_t value,const char *error);
} GpuEngineState;
/* A rejected configuration write leaves engine state unchanged. A submitted
 * draw with unknown completion retains pending work. Method is a word index. */
static const char *gpu_engine_apply(GpuEngineState *engine,unsigned sub,unsigned method,uint32_t value) {
 if(sub>=8 || method>=4096) return "engine method range";
 /* GF100+ host NOP, byte 0x08: envytools rnndb/fifo/nv_object.xml.
  * Payload is unrestricted and has no completion or engine-state effect. */
 if(method==2) return NULL;
 /* NVIDIA clb06f.h SET_REFERENCE, byte 0x50, full 32-bit count.
  * Channel-wide ordered update, not a guest syncpoint/semaphore increment.
  * Do not publish completion while asynchronous work or inline data remains. */
 if(method==0x50/4) {
  if(engine->pending_work || engine->inline_dma.active) return "host reference requires completion";
  if(engine->host_reference_writes==UINT64_MAX) return "host reference counter exhausted";
  atomic_thread_fence(memory_order_seq_cst);
  engine->host_reference=value;engine->host_reference_valid=1;engine->host_reference_writes++;
  return NULL;
 }
 /* GM20B retains MEM_OP_B at byte 0x2c (deko3d engine_gpfifo.def).
  * Host methods are channel-wide and do not require a subchannel binding.
  * All supported memory stores currently target coherent CPU backing.
  * A future cached/asynchronous backend must extend this completion path. */
 if(method==0x0b) {
  if(value!=0x80000000u && value!=0x70000000u) return "unsupported host memory operation";
  if(engine->pending_work || engine->inline_dma.active) return "host memory operation requires completion";
  uint64_t *count=value==0x80000000u ? &engine->host_l2_flushes : &engine->host_l2_invalidations;
  if(*count==UINT64_MAX) return "host memory operation counter exhausted";
  atomic_thread_fence(memory_order_seq_cst);(*count)++;return NULL;
 }
 if(!method) {
  switch(value) {
   case 0xb197:case 0xb1c0:case 0xb0b5:case 0xa140:case 0x902d:case 0xb06f:break;
   default:return "unsupported engine binding";
  }
  engine->classes[sub]=value;return NULL;
 }
 if(engine->classes[sub]==0x902d) {
  /* Configuration only; no copy, blend or memory transfer is performed here.
   * Field definitions: NVIDIA open-gpu-doc classes/twod/cl902d.h. */
  uint32_t *target;unsigned bit;
  switch(method*4) {
   case 0x2ac:
    if(value>6) return "unsupported 2D operation";
    target=&engine->twod.operation;bit=1;break;
   case 0x290:
    if(value>1) return "unsupported 2D clip enable";
    target=&engine->twod.clip_enable;bit=2;break;
   case 0x884:
    if(value>1023) return "unsupported 2D corral size";
    target=&engine->twod.corral_size;bit=4;break;
   default:return "unsupported 2D method";
  }
  if(!(engine->twod.valid&bit) || *target!=value) engine->twod.dirty|=bit;
  *target=value;engine->twod.valid|=bit;return NULL;
 }
 if(engine->classes[sub]==0xb1c0) return gpu_compute_write(&engine->compute,method,value);
 if(engine->classes[sub]!=0xb197) return "unsupported engine method";
 /* B197 PIXEL_SHADER_BARRIER. Native draws return only after Vulkan fence
  * completion and canonical guest writeback; no GPU stores remain deferred.
  * Never use a CPU fence as a substitute for unfinished GPU completion. */
 if(method==0x0de0/4) {
  if(value>1)return "unsupported pixel shader barrier bits";
  if(engine->pending_work || engine->inline_dma.active)return "pixel shader barrier requires completion";
  if(engine->pixel_shader_barriers[value]==UINT64_MAX)return "pixel shader barrier counter exhausted";
  atomic_thread_fence(memory_order_seq_cst);engine->pixel_shader_barriers[value]++;return NULL;
 }
 if(method==0x1618/4) {
  if(value>14)return "unsupported begin topology or continuation flags";
  if(engine->draw.active)return "nested graphics begin";
  if(engine->pending_work || engine->inline_dma.active)return "graphics begin requires completion";
  engine->draw.active=1;engine->draw.topology=value;
  engine->draw.instance_id=0;engine->draw.primitive_id=0;return NULL;
 }
 if(method==0x1614/4) {
  if(value)return "unsupported graphics end bits";
  if(!engine->draw.active)return "graphics end without begin";
  if(engine->pending_work || engine->inline_dma.active)return "graphics end requires completion";
  engine->draw.active=0;return NULL;
 }
 if(method==0x17e0/4) {
  if(!engine->draw.active)return "indexed draw without begin";
  if(engine->pending_work || engine->inline_dma.active)return "indexed draw requires completion";
  const char *error="indexed draw backend unavailable";
  if(engine->indexed_draw) {
   engine->pending_work++;
   GpuDrawResult result=engine->indexed_draw(engine,value);
   if(!result.completion_unknown)engine->pending_work--;
   error=result.error;
   if(result.completion_unknown && !error)error="indexed draw completion unknown";
   if(!error){engine->indexed_draws++;return NULL;}
  }
  if(engine->indexed_draw_failure_report)
   engine->indexed_draw_failure_report(engine->memory_context,&engine->graphics,value);
  return error;
 }
 /* deko3d engine_3d.def 0x35E DrawArraysCount (byte 0x0d78) launches a
  * non-indexed draw from DrawArraysFirst. Count is not retained as state. */
 if(method==0x0d78/4) {
  if(!engine->draw.active)return "non-indexed draw without begin";
  if(engine->pending_work || engine->inline_dma.active)return "non-indexed draw requires completion";
  if(!engine->graphics.valid[0x0d74/4])return "non-indexed draw first vertex unknown";
  if(!engine->vertex_draw)return "non-indexed draw backend unavailable";
  if(engine->vertex_draws==UINT64_MAX)return "non-indexed draw counter exhausted";
  engine->pending_work++;
  GpuDrawResult result=engine->vertex_draw(engine,engine->graphics.values[0x0d74/4],value);
  if(!result.completion_unknown)engine->pending_work--;
  if(result.completion_unknown && !result.error)result.error="non-indexed draw completion unknown";
  if(!result.error)engine->vertex_draws++;
  return result.error;
 }
 /* clb197.h NO_OPERATION at byte 0x100 accepts all 32 payload bits.
  * It neither waits for work nor modifies graphics or completion state. */
 if(method==0x100/4)return NULL;
 if(method==0x1464/4 || method==0x1500/4) {
  if(value)return "unsupported ZCULL store/load payload";
  if(!engine->conservative_zcull)return "ZCULL backend policy not configured";
  if(engine->pending_work || engine->inline_dma.active)return "ZCULL store/load requires completion";
  uint32_t fields[4];
  for(unsigned i=0;i<4;i++) {
   const char *error=gpu_graphics_read(&engine->graphics,0x7e8/4+i,&fields[i]);if(error)return error;
  }
  uint64_t start=((uint64_t)fields[0]<<32)|fields[1],limit=((uint64_t)fields[2]<<32)|fields[3];
  if(!start || limit<start)return "invalid ZCULL storage interval";
  uint64_t *count=method==0x1464/4?&engine->zcull_stores:&engine->zcull_loads;
  if(*count==UINT64_MAX || engine->graphics.zcull_invalidation_serial==UINT64_MAX)
   return "ZCULL operation counter exhausted";
  /* This native backend has no hierarchical cache to serialize or restore.
   * Never synthesize opaque NVIDIA bytes or let an old hint reject a fragment.
   * Store/load preserve the depth image and force conservative culling. */
  atomic_thread_fence(memory_order_seq_cst);
  engine->zcull_storage_start=start;engine->zcull_storage_limit=limit;
  engine->graphics.zcull_invalidation_serial++;(*count)++;return NULL;
 }
 if(method==0x4b2) {
  /* deko3d ConditionalZcullInvalidate emits this no-WFI action with 0x19.
   * There is no host ZCULL cache; publish invalidation for future backends.
   * Other control values and asynchronous outstanding work remain unsupported. */
  if(value!=0x19) return "unsupported ZCULL invalidation control";
  if(engine->pending_work || engine->inline_dma.active) return "ZCULL invalidation work outstanding";
  if(engine->graphics.zcull_invalidation_serial==UINT64_MAX) return "ZCULL invalidation serial exhausted";
  engine->graphics.zcull_invalidation_serial++;return NULL;
 }
 if(method==0x19d0/4) {
  if(engine->pending_work || engine->inline_dma.active) return "color clear requires completion";
  if(!engine->clear_color) return "color clear backend unavailable";
  uint64_t *count=(value&3) ? &engine->depth_stencil_clears : &engine->color_clears;
  if(*count==UINT64_MAX) return "surface clear counter exhausted";
  GpuColorClear clear;const char *error=(value&3) ? gpu_depth_clear_description(&engine->graphics,value,&clear) : gpu_color_clear_description(&engine->graphics,value,&clear);if(error) return error;
  error=engine->clear_color(engine->memory_context,&clear);if(error) return error;
  atomic_thread_fence(memory_order_seq_cst);(*count)++;return NULL;
 }
 if(method==0x2e0/4) {
  /* DECOMPRESS_SURFACE is an ordered action, never retained configuration.
   * Only single-layer, single-sample RGBA8 block-linear targets are supported.
   * The callback must establish canonical storage, not merely mapped bytes. */
  if(value&~7u) return "unsupported surface decompression layer or bits";
  if(engine->pending_work || engine->inline_dma.active) return "surface decompression requires completion";
  if(!engine->decompress_surface) return "surface decompression backend unavailable";
  uint32_t fields[8],samples;unsigned start=0x200+(value&7)*16;
  for(unsigned i=0;i<8;i++) {const char *error=gpu_graphics_read(&engine->graphics,start+i,&fields[i]);if(error) return error;}
  const char *error=gpu_graphics_read(&engine->graphics,0x15d0/4,&samples);if(error) return error;
  if(samples || fields[4]!=0xd5 || fields[6]!=1 || (fields[5]&~0x70u) || ((fields[5]>>4)&7)>5)
   return "unsupported surface decompression layout";
  uint64_t width=fields[2],height=fields[3],block_height=8ull<<((fields[5]>>4)&7);
  uint64_t address=((uint64_t)fields[0]<<32)|fields[1],stride=(uint64_t)fields[7]*4;
  uint64_t extent=((width*4+63)&~63ull)*((height+block_height-1)&~(block_height-1));
  if(!width || !height || (address&511) || (stride&511) || stride<extent || extent>0x10000000000ull-address)
   return "invalid surface decompression extent";
  if(engine->surface_decompressions==UINT64_MAX) return "surface decompression counter exhausted";
  error=engine->decompress_surface(engine->memory_context,address,extent);if(error) return error;
  atomic_thread_fence(memory_order_seq_cst);engine->surface_decompressions++;return NULL;
 }
 if(method==0x2c8/4) {
  /* clb197.h: index[11:0], CLEAN_L2[16], condition[20]. Both
   * completion conditions are satisfied only after all supported work ends. */
  if(value&~0x110fffu) return "unsupported syncpoint increment bits";
  if(engine->pending_work || engine->inline_dma.active) return "syncpoint increment requires completion";
  if(!engine->increment_syncpoint) return "syncpoint service unavailable";
  return engine->increment_syncpoint(engine->syncpoint_context,value&4095);
 }
 if(method>=0x1b00/4 && method<=0x1b08/4) {
  unsigned index=method-0x1b00/4;
  if(!index && value>255) return "report semaphore address upper bits";
  engine->report_semaphore[index]=value;engine->report_semaphore_valid|=1u<<index;return NULL;
 }
 if(method==0x1b0c/4) {
  /* One-word payload release after all preceding writes, all pipeline stages.
   * Queries, timestamps, reductions, acquisition and awaken remain unsupported. */
  if(value!=0x1000f010u) return "unsupported report semaphore operation";
  if(engine->report_semaphore_valid!=7) return "report semaphore configuration missing";
  if(engine->pending_work || engine->inline_dma.active) return "report semaphore requires completion";
  uint64_t address=((uint64_t)engine->report_semaphore[0]<<32)|engine->report_semaphore[1];
  if(address&3) return "report semaphore alignment";
  if(!engine->write_word) return "report semaphore memory unavailable";
  if(engine->report_semaphore_releases==UINT64_MAX) return "report semaphore counter exhausted";
  atomic_thread_fence(memory_order_seq_cst);
  const char *error=engine->write_word(engine->memory_context,address,engine->report_semaphore[2]);
  if(error) return error;
  engine->report_semaphore_releases++;return NULL;
 }
 unsigned cache_slot=6;
 switch(method*4) {
  case 0x0f74:cache_slot=0;break; /* GM20B/deko3d: texture invalidate with implicit wait */
  case 0x1288:cache_slot=0;break; /* texture data, all lines only */
  case 0x0da4:cache_slot=1;break; /* shader instruction/global/constant */
  case 0x1428:cache_slot=2;break; /* texture headers, all lines only */
  case 0x1424:cache_slot=3;break; /* samplers, all lines only */
  case 0x1144:cache_slot=4;break; /* flush pending stores */
  case 0x0f80:cache_slot=5;break; /* deko3d GM20B TiledCacheFlush, Flush=0 */
 }
 if(cache_slot<6) {
  /* NVIDIA clb197.h. Native draws synchronously finish and write back into
   * coherent CPU backing; no persistent GPU resources are cached. Do not
   * acknowledge these operations while any work remains pending. */
  uint32_t mask=cache_slot==1 ? 0x1011u : cache_slot==4 ? 1u : 0u;
  if(value&~mask) return "unsupported graphics cache operation bits";
  if(engine->pending_work || engine->inline_dma.active) return "graphics cache operation requires completion";
  if(engine->cache_operations[cache_slot]==UINT64_MAX) return "graphics cache operation counter exhausted";
  if(method==0x0f74/4 && engine->texture_wait_invalidations==UINT64_MAX)return "texture wait invalidation counter exhausted";
  atomic_thread_fence(memory_order_seq_cst);
  if(method==0x0f74/4)engine->texture_wait_invalidations++;
  engine->cache_operations[cache_slot]++;return NULL;
 }
 if(method>=0x904 && method<=0x924 && !((method-0x904)&7)) {
  if(value&~0x1f1u) return "constant buffer binding bits";
  unsigned stage=(method-0x904)/8,slot=(value>>4)&31,enabled=value&1;
  uint64_t address=0;uint32_t size=0;
  if(enabled) {
   for(unsigned i=0x8e0;i<=0x8e2;i++) if(!engine->graphics.valid[i]) return "constant buffer binding selector missing";
   address=((uint64_t)engine->graphics.values[0x8e1]<<32)|engine->graphics.values[0x8e2];
   size=engine->graphics.values[0x8e0];
   if(!size || (address&255) || size>0x10000000000ull-address) return "unsupported constant buffer binding extent";
  }
  /* Bind captures the selector; later selector changes do not rebind slots.
   * Mapping and read permissions are checked when a shader consumes it. */
  if(!engine->constant_buffers[stage][slot].known || engine->constant_buffers[stage][slot].enabled!=enabled ||
     engine->constant_buffers[stage][slot].address!=address || engine->constant_buffers[stage][slot].size!=size)
   engine->constant_buffers[stage][slot].dirty=1;
  engine->constant_buffers[stage][slot].address=address;engine->constant_buffers[stage][slot].size=size;
  engine->constant_buffers[stage][slot].known=1;engine->constant_buffers[stage][slot].enabled=enabled;
  return NULL;
 }
 if(method>=0x60 && method<=0x63) {
  if(engine->inline_dma.active) return "inline DMA configuration while active";
  unsigned slot=method-0x60;
  if(slot==2 && value>255) return "inline DMA address upper bits";
  switch(slot) {
   case 0:engine->inline_dma.length=value;break;
   case 1:engine->inline_dma.lines=value;break;
   case 2:engine->inline_dma.upper=value;break;
   case 3:engine->inline_dma.lower=value;break;
  }
  engine->inline_dma.valid|=1u<<slot;return NULL;
 }
 if(method==0x6c) {
  if(engine->inline_dma.active || engine->pending_work) return "inline DMA engine busy";
  /* Pitch, flush-only, no interrupt/reduction/semaphore release. The size
   * flag is immaterial with no semaphore, but both documented values occur. */
  if(value!=0x11 && value!=0x1011) return "unsupported inline DMA launch flags";
  if(engine->inline_dma.valid!=15) return "inline DMA configuration missing";
  if(engine->inline_dma.lines!=1) return "unsupported inline DMA line count";
  uint64_t address=((uint64_t)engine->inline_dma.upper<<32)|engine->inline_dma.lower;
  uint32_t length=engine->inline_dma.length;
  if(!length || (length&3) || (address&3)) return "unsupported inline DMA alignment or length";
  if(length>0x10000000000ull-address) return "inline DMA address overflow";
  if(!engine->write_word || !engine->check_write) return "inline DMA memory access unavailable";
  if(engine->inline_dma.completed==UINT64_MAX || length/4>UINT64_MAX-engine->inline_dma.words)
   return "inline DMA counter exhausted";
  const char *error=engine->check_write(engine->memory_context,address,length);if(error) return error;
  engine->inline_dma.address=address;engine->inline_dma.remaining=length;
  engine->inline_dma.active=1;engine->pending_work=1;return NULL;
 }
 if(method==0x6d) {
  if(!engine->inline_dma.active || engine->inline_dma.remaining<4) return "inline DMA data without launch";
  if(!engine->write_word || engine->pending_work!=1) return "inline DMA execution state";
  const char *error=engine->write_word(engine->memory_context,engine->inline_dma.address,value);
  if(error) return error;
  engine->inline_dma.address+=4;engine->inline_dma.remaining-=4;engine->inline_dma.words++;
  if(!engine->inline_dma.remaining) {
   /* All synchronous stores are visible. No queued hardware work or GPU cache
    * exists yet; completion does not signal any guest fence/semaphore. */
   engine->inline_dma.active=0;engine->pending_work=0;engine->inline_dma.completed++;
  }
  return NULL;
 }
 if(method>=0x8e4 && method<0x8f4) {
  GpuGraphicsState *g=&engine->graphics;
  for(unsigned i=0x8e0;i<=0x8e3;i++) if(!g->valid[i]) return "constant buffer selector not established";
  uint64_t base=((uint64_t)g->values[0x8e1]<<32)|g->values[0x8e2];
  uint32_t size=g->values[0x8e0],offset=g->values[0x8e3];
  if((base&255) || (offset&3)) return "unsupported constant buffer alignment";
  if(offset>size || size-offset<4) return "constant buffer upload extent";
  /* Do not guess how the 16-bit offset register wraps at its end. */
  if(offset>65528) return "constant buffer offset wrap unresolved";
  if(base>0xffffffffffull-offset-3) return "constant buffer address overflow";
  if(engine->pending_work) return "constant buffer upload requires backend completion";
  if(!engine->write_word) return "constant buffer memory writer unavailable";
  if(engine->constant_buffer_words==UINT64_MAX) return "constant buffer upload counter exhausted";
  const char *error=engine->write_word(engine->memory_context,base+offset,value);
  if(error) return error;
  g->values[0x8e3]=offset+4;g->dirty[0x8e3]=1;
  engine->constant_buffer_words++;return NULL;
 }
 switch(method) {
  case 0x1a2c/4:
   if(value) return "unsupported pipe NOP payload";
   return NULL;
  case 0x150/4:case 0x154/4:
   /* Established initialization markers from deko3d gpu_3d_base.cpp.
    * Other instrumentation payloads require a separate interpretation. */
   if(value!=(method==0x150/4 ? 0x49000000u : 0x49000001u)) return "unsupported instrumentation payload";
   if(engine->instrumentation_count>=GPU_INSTRUMENTATION_RECORDS) return "instrumentation record limit";
   engine->instrumentation[engine->instrumentation_count].method=method;
   engine->instrumentation[engine->instrumentation_count++].value=value;
   return NULL;
  case 0x2310/4: {
   /* Falcon masked context-register write. Protocol: deko3d WriteHardwareReg
    * and Mesa mme_set_priv_reg. Only reviewed context storage is accepted. */
   if(engine->pending_work) return "Falcon write requires backend completion";
   struct GpuMaskedConfig *target;uint32_t allowed;
   switch(value) {
    case 0x418800:target=&engine->setup_config;allowed=0x01800001;break;
    case 0x419a04:target=&engine->tex_config04;allowed=3;break;
    case 0x419a08:target=&engine->tex_config08;allowed=0x10;break;
    case 0x419f78:target=&engine->sm_config178;allowed=8;break;
    case 0x404468:target=&engine->macro_watchdog;allowed=0x3fffffff;break;
    case 0x418e40:target=&engine->tile_config[0];allowed=0xffffffffu;break;
    case 0x418e44:target=&engine->tile_config[1];allowed=0xfff;break;
    case 0x418e58:case 0x418e5c:case 0x418e60:case 0x418e64:case 0x418e68:case 0x418e6c:
     target=&engine->tile_config[2+(value-0x418e58)/4];
     allowed=value==0x418e6c ? 0xffff : 0xffffffffu;break;
    default:return "unsupported Falcon register";
   }
   if(!engine->graphics.valid[0xd00] || !engine->graphics.valid[0xd01] ||
      !engine->graphics.valid[0xd02]) return "Falcon arguments not established";
   if(engine->graphics.values[0xd00]) return "Falcon acknowledgement not cleared";
   uint32_t bits=engine->graphics.values[0xd01],mask=engine->graphics.values[0xd02];
   if(!mask || (mask&~allowed)) return "unsupported Falcon register mask";
   uint32_t next=(target->value&~mask)|(bits&mask);
   target->dirty|=mask&(~target->known|(next^target->value));
   target->value=next;target->known|=mask;
   /* Synchronous state update completed; queue/fence state is unaffected. */
   engine->graphics.values[0xd00]=1;engine->graphics.dirty[0xd00]=1;
   return NULL;
  }
  case 0x44:
   if(value) return "unsupported wait-for-idle argument";
   if(engine->pending_work) return "engine wait requires backend completion";
   /* Empty engine work is already idle. Do not change queue/fence counters. */
   return NULL;
  case 0x5aa:case 0x5e5: {
   /* GM20B's initialization contract, also used by Mesa/Nouveau.
    * Only these exact version ranges are established; other ranges need
    * separate compatibility validation, not an invented overlap rule. */
   unsigned expected=method==0x5aa ? 3 : 2;
   if((value&65535)!=(value>>16)) return "unsupported GPU version range";
   if((value&65535)!=expected) return "unsupported GPU format version";
   if(method==0x5aa) {engine->sph_version_check=value;engine->sph_checked=1;}
   else {engine->aam_version_check=value;engine->aam_checked=1;}
   return NULL;
  }
  case 0x45:
   if(value>=GPU_MME_WORDS) return "MME instruction pointer limit";
   engine->code_pointer=value;return NULL;
  case 0x46:
   if(engine->code_pointer>=GPU_MME_WORDS) return "MME instruction RAM limit";
   engine->code[engine->code_pointer]=value;engine->code_valid[engine->code_pointer++]=1;
   engine->code_writes++;return NULL;
  case 0x47:
   if(value>=GPU_MME_ENTRIES) return "MME entry pointer limit";
   engine->start_pointer=value;return NULL;
  case 0x48:
   if(engine->start_pointer>=GPU_MME_ENTRIES || value>=GPU_MME_WORDS) return "MME entry RAM limit";
   /* Entry upload may precede its code. Validity is checked on invocation. */
   engine->starts[engine->start_pointer]=value;engine->start_valid[engine->start_pointer++]=1;
   engine->start_writes++;return NULL;
  default:return gpu_graphics_write(&engine->graphics,method,value);
 }
}
/* deko3d's decompressSurface saves ordinary render-target state across a
 * passthrough/replay bracket while normal operation uses filtered tracking.
 * Initialization writes window origin before its first explicit mode change,
 * so these reviewed fields also retain startup writes. Unwritten state and
 * the initial mode itself remain unknown; this is not a full reset profile. */
static int gpu_shadow_filtered_tracks(unsigned method) {
 if(method>=0x200 && method<0x280 && (method&15)<8) return 1;
 /* deko3d clearDepthStencil temporarily overrides and replays the front mask. */
 return method==0x139c/4 || method==0x15d0/4 || method==0xff4/4 || method==0xff8/4 || method==0x13ac/4 || method==0xfb4/4;
}
/* Shadow recording is committed only after a successful method. Control and
 * low methods bypass shadowing; macro parameters are dispatched separately.
 * For unreviewed filter membership, mode 1 preserves knowledge only when
 * tracking and filtering-out would produce the same saved value. */
static const char *gpu_engine_direct(GpuEngineState *engine,unsigned sub,unsigned method,uint32_t value) {
 if(sub>=8 || method>=4096) return "engine method range";
 if(engine->classes[sub]!=0xb197) return gpu_engine_apply(engine,sub,method,value);
 if(method==0x49) {
  if(value>3) return "unsupported shadow RAM mode";
  engine->shadow_mode=value;engine->shadow_mode_known=1;return NULL;
 }
 unsigned startup_track=!engine->shadow_mode_known && gpu_shadow_filtered_tracks(method);
 unsigned shadowed=method>=0x80 && method<GPU_GRAPHICS_METHODS && (engine->shadow_mode_known || startup_track);
 uint32_t effective=value;
 if(shadowed && engine->shadow_mode==3) {
  if(!engine->shadow_known[method]) return "shadow replay value unknown";
  effective=engine->shadow_values[method];
 }
 const char *error=gpu_engine_apply(engine,sub,method,effective);if(error) return error;
 if(shadowed && (startup_track || engine->shadow_mode==0 || (engine->shadow_mode==1 && gpu_shadow_filtered_tracks(method)))) {
  engine->shadow_values[method]=value;engine->shadow_known[method]=1;
 } else if(shadowed && engine->shadow_mode==1 &&
           (!engine->shadow_known[method] || engine->shadow_values[method]!=value)) {
  engine->shadow_known[method]=0;
 }
 return NULL;
}
static const char *gpu_macro_engine_read(void *opaque,unsigned method,uint32_t *value) {
 GpuEngineState *engine=opaque;
 if(method==0x49 && engine->shadow_mode_known) {*value=engine->shadow_mode;return NULL;}
 return gpu_graphics_read(&engine->graphics,method,value);
}
static const char *gpu_macro_engine_emit(void *opaque,unsigned method,uint32_t value) {
 GpuEngineState *engine=opaque;
 const char *error;
 if(!method || (method>=0x45 && method<=0x48) || method>=0xe00)error="unsupported macro control method";
 else error=gpu_engine_direct(engine,engine->macro_subchannel,method,value);
 if(error && engine->macro_failure_report)engine->macro_failure_report(method,value,error);
 return error;
}
/* Macro dispatch may apply several instructions before a terminal error. Those
 * effects persist; the error is sticky and the submission must not be retried. */
static const char *gpu_engine_method(GpuEngineState *engine,unsigned sub,unsigned method,uint32_t value) {
 if(sub>=8 || method>=4096) return "engine method range";
 if(engine->macro_error) return engine->macro_error;
 if(engine->macro_active) {
  if(sub!=engine->macro_subchannel || method!=0xe01+engine->macro_slot*2) return "macro parameter expected";
  if(!engine->macro.waiting || engine->macro.has_parameter) return "macro parameter slot unavailable";
  engine->macro.parameter=value;engine->macro.has_parameter=1;engine->macro.waiting=0;
 } else if(engine->classes[sub]==0xb197 && method>=0xe00) {
  unsigned slot=(method-0xe00)/2;
  if(method&1) return "macro data without invocation";
  if(slot>=GPU_MME_ENTRIES || !engine->start_valid[slot]) return "macro entry not uploaded";
  unsigned pc=engine->starts[slot];
  if(pc>=GPU_MME_WORDS || !engine->code_valid[pc]) return "macro entry instruction not uploaded";
  gpu_macro_begin(&engine->macro,pc,value);
  engine->macro_active=1;engine->macro_slot=slot;engine->macro_subchannel=sub;
 } else return gpu_engine_direct(engine,sub,method,value);
 engine->macro_error=gpu_macro_run(&engine->macro,engine->code,engine->code_valid,GPU_MME_WORDS,
  gpu_macro_engine_read,gpu_macro_engine_emit,engine);
 if(!engine->macro_error && engine->macro.done) engine->macro_active=0;
 return engine->macro_error;
}
