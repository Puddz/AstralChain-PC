/* Typed, server-owned descriptors. A cloned transport shares this namespace. */
enum { NV_DEVICE_CTRL_GPU=1, NV_DEVICE_AS_GPU=2, NV_DEVICE_MAP=3, NV_DEVICE_CHANNEL=4 };
#include "gpu_model.h"
#include "gpu_zbc.h"
#include "gpu_address_space.h"
#include "nvmap.h"
#include "gpu_mapping.h"
#include "gpu_channel.h"
#include "gpu_packets.h"
#include "gpu_shader_capture.h"
#include "gpu_r32_surface.h"
static const char *gpu_store_canonical_r32(void *opaque,const GpuR32Surface *surface,
 const void *pixels,size_t size) {
 const char *error=gpu_r32_layout(surface);if(error)return error;
 if(!opaque)return "RF32 address space unavailable";
 GpuMemoryView view=gpu_resolve((GpuAddressSpace*)opaque,surface->address,surface->extent);
 if(!view.bytes || (view.kind!=0xfe && view.kind!=0xdb) || view.storage!=GPU_STORAGE_CANONICAL)
  return "RF32 destination mapping or storage unavailable";
 uint64_t cpu=(uint64_t)(uintptr_t)view.bytes;Region r=query(cpu);
 if(!(r.permission&2) || cpu<r.base || cpu-r.base>=r.size || surface->extent>r.size-(cpu-r.base))
  return "RF32 destination backing not writable";
 return gpu_r32_pack(surface,view.bytes,(size_t)surface->extent,pixels,size);
}
#include "gpu_experimental_draw.h"
static const char *gpu_check_mapped_write(void *opaque,uint64_t address,uint64_t size) {
 if(!opaque) return "GPU upload address space unavailable";
 GpuMemoryView view=gpu_resolve((GpuAddressSpace*)opaque,address,size);
 if(!view.bytes) return "GPU upload unmapped extent";
 if(view.kind) return "GPU upload requires pitch mapping";
 uint64_t cpu=(uint64_t)(uintptr_t)view.bytes;Region r=query(cpu);
 if(!(r.permission&2) || cpu<r.base || cpu-r.base>=r.size || size>r.size-(cpu-r.base))
  return "GPU upload backing not writable";
 return NULL;
}
static const char *gpu_write_mapped_word(void *opaque,uint64_t address,uint32_t value) {
 const char *error=gpu_check_mapped_write(opaque,address,4);if(error) return error;
 GpuMemoryView view=gpu_resolve((GpuAddressSpace*)opaque,address,4);
 /* Guest and host targets are little endian; write only after all checks. */
 uint8_t *p=view.bytes;for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(value>>(i*8));
 return NULL;
}
static const char *gpu_decompress_canonical_surface(void *opaque,uint64_t address,uint64_t size) {
 if(!opaque) return "surface address space unavailable";
 GpuMemoryView view=gpu_resolve((GpuAddressSpace*)opaque,address,size);
 if(!view.bytes) return "surface unmapped or mixed-kind extent";
 if(view.kind!=0xfe && view.kind!=0xdb) return "unsupported surface mapping kind";
 if(view.storage!=GPU_STORAGE_CANONICAL) return "surface storage requires compression decoder";
 uint64_t cpu=(uint64_t)(uintptr_t)view.bytes;Region r=query(cpu);
 if(!(r.permission&1) || cpu<r.base || cpu-r.base>=r.size || size>r.size-(cpu-r.base)) return "surface backing not readable";
 /* All current producers store canonical block-linear bytes. Compression-
  * eligible mappings do not compress them. No rewrite is needed or performed;
  * this does not decode foreign compressed data or untile the surface. */
 return NULL;
}
static const char *gpu_clear_canonical_color(void *opaque,const GpuColorClear *c) {
 if(!c->packed_mask)return NULL; /* No enabled aspect/component can change memory. */
 if(!opaque) return "color clear address space unavailable";
 GpuMemoryView view=gpu_resolve((GpuAddressSpace*)opaque,c->address,c->extent);
 if(!view.bytes) return "color clear unmapped or mixed-kind extent";
 if((view.kind!=0xfe && view.kind!=(c->depth_stencil ? (c->depth_format==0x0a ? 0x86 : 0x51) : 0xdb)) || view.storage!=GPU_STORAGE_CANONICAL) return "color clear storage unsupported";
 uint64_t cpu=(uint64_t)(uintptr_t)view.bytes;Region r=query(cpu);
 if((r.permission&3)!=3 || cpu<r.base || cpu-r.base>=r.size || c->extent>r.size-(cpu-r.base)) return "color clear backing permissions";
 uint8_t *bytes=view.bytes;uint64_t rows=8ull<<c->block_height;
 /* All extent/format checks precede the first write. Partial component masks
  * preserve other channels, and padding outside the rectangle is untouched. */
 for(unsigned y=c->top;y<c->bottom;y++) for(unsigned x=c->left;x<c->right;x++) {
  uint64_t xb=(uint64_t)x*4;
  uint64_t at=(y/rows)*rows*c->pitch+(xb/64)*512*(1ull<<c->block_height)+((y%rows)/8)*512;
  at+=((xb%64)/32)*256+((y%8)/2)*64+((xb%32)/16)*32+(y%2)*16+xb%16;
  uint32_t previous=(uint32_t)bytes[at]|(uint32_t)bytes[at+1]<<8|(uint32_t)bytes[at+2]<<16|(uint32_t)bytes[at+3]<<24;
  uint32_t value=(previous&~c->packed_mask)|(c->packed_color&c->packed_mask);
  for(unsigned k=0;k<4;k++) bytes[at+k]=(uint8_t)(value>>(k*8));
 }
 return NULL;
}
static const char *gpu_check_descriptor(GpuAddressSpace *space,uint64_t entry,unsigned *total_words) {
 unsigned high=entry>>32,count=(high>>10)&0x1fffff;
 if(!count) {
  /* Control entry: only NOP opcode, operand ignored, SYNC still ordered. */
  return (high&0x1ff) ? "gpu-control-descriptor" : NULL;
 }
 if((entry&3) || (high&0x100)) return "gpu-pushbuffer-options";
 if(!space) return "gpu-pushbuffer-address-space";
 uint64_t address=(entry&0xfffffffcull)|((uint64_t)(high&255)<<32),size=(uint64_t)count*4;
 GpuMemoryView view=gpu_resolve(space,address,size);
 if(!view.bytes) return "gpu-pushbuffer-memory";
 if(view.kind) return "gpu-pushbuffer-options";
 uint64_t cpu=(uint64_t)(uintptr_t)view.bytes;Region r=query(cpu);
 if(!(r.permission&1) || cpu<r.base || cpu-r.base>=r.size || size>r.size-(cpu-r.base)) return "gpu-pushbuffer-readable";
 if(count>65536-*total_words) return "gpu-pushbuffer-decode-budget";
 *total_words+=count;return NULL;
}
typedef struct {
 unsigned classes[8],writes,executed;
 GpuEngineState *engine;
 const char *execution_error;
} GpuPacketAudit;
/* Capture effective state after macro-generated writes, not only the outer
 * pushbuffer inputs. Validity masks distinguish unknown values from zero. */
static void gpu_audit_graphics_state(const GpuEngineState *engine) {
 const GpuGraphicsState *g=&engine->graphics;
 printf("GPU effective draw active=%u topology=%u instance=%u primitive=%u\n",
  engine->draw.active,engine->draw.topology,engine->draw.instance_id,engine->draw.primitive_id);
 for(unsigned slot=0;slot<6;slot++) {
  unsigned base=0x2000/4+slot*16,valid=0;
  for(unsigned field=0;field<5;field++)if(g->valid[base+field])valid|=1u<<field;
  if(!valid)continue;
  printf("GPU effective shader slot=%u valid=0x%02x fields=",slot,valid);
  for(unsigned field=0;field<5;field++)printf("%s%08x",field?",":"",g->values[base+field]);
  printf("\n");
 }
 for(unsigned stage=0;stage<5;stage++)for(unsigned slot=0;slot<32;slot++) {
  if(!engine->constant_buffers[stage][slot].known)continue;
  printf("GPU effective cbuf stage=%u slot=%u enabled=%u address=0x%llx size=0x%x\n",
   stage,slot,engine->constant_buffers[stage][slot].enabled,
   (unsigned long long)engine->constant_buffers[stage][slot].address,engine->constant_buffers[stage][slot].size);
 }
 for(unsigned target=0;target<8;target++) {
  unsigned valid=0;for(unsigned i=0;i<8;i++)if(g->valid[0x200+target*16+i])valid|=1u<<i;
  if(!valid)continue;
  printf("GPU effective color target=%u valid=0x%02x fields=",target,valid);
  for(unsigned i=0;i<8;i++)printf("%s%08x",i?",":"",g->values[0x200+target*16+i]);
  printf("\n");
 }
 const unsigned methods[]={0xfe0,0xfe4,0xfe8,0xfec,0xff0,0x1228,0x122c,0x1230,0x1538,
  0x10f8,0x13ac,0x15cc,0x15d0,0xff4,0xff8,0xd6c,0xd70,0xe00,0xe04,0xe08,0xc00,0xc04,
  0xd80,0xd84,0xd88,0xd8c,0xd90,0xda0,0x139c,0x1608,0x160c,
  0x17c8,0x17cc,0x17d0,0x17d4,0x17d8,0x17dc,
  0x2608,0x1234,0x155c,0x1560,0x1564,0x1574,0x1578,0x157c};
 for(unsigned i=0;i<sizeof(methods)/sizeof(methods[0]);i++) {
  unsigned method=methods[i]/4;
  printf("GPU effective register offset=0x%x valid=%u value=0x%08x\n",methods[i],g->valid[method],g->values[method]);
 }
}
static void gpu_audit_macro_failure(unsigned method,uint32_t value,const char *error) {
 printf("GPU macro emit failed offset=0x%x value=0x%08x reason=%s\n",method*4,value,error);
}
static int gpu_audit_method(void *opaque,unsigned subchannel,unsigned method,uint32_t value) {
 GpuPacketAudit *audit=opaque;
 if(!method) audit->classes[subchannel]=value&65535;
 printf("GPU method sub=%u class=0x%x offset=0x%x value=0x%08x\n",subchannel,audit->classes[subchannel],method*4,value);
 if(audit->engine && !audit->execution_error) {
  audit->execution_error=gpu_engine_method(audit->engine,subchannel,method,value);
  if(audit->execution_error) {
   printf("GPU execution boundary sub=%u offset=0x%x reason=%s\n",subchannel,method*4,audit->execution_error);
   gpu_audit_graphics_state(audit->engine);
   gpu_capture_shader_windows(audit->engine);
   gpu_capture_bound_buffers(audit->engine);
   if(audit->engine->macro_active) printf("GPU macro slot=%u pc=%u steps=%u waiting=%u\n",audit->engine->macro_slot,
    audit->engine->macro.pc,audit->engine->macro.steps,audit->engine->macro.waiting);
  }
  else {
   audit->executed++;
   if(method==0x1464/4 || method==0x1500/4)
    printf("GPU ZCULL conservative %s start=0x%llx limit=0x%llx\n",method==0x1464/4?"store":"load",
     (unsigned long long)audit->engine->zcull_storage_start,(unsigned long long)audit->engine->zcull_storage_limit);
  }
 }
 audit->writes++;return 1;
}
static struct {
 unsigned fd,owner,kind,open,refs,channel_space,channel_nvmap;
 HANDLE events[3];GpuAddressSpace address_space;
 GpuChannel channel;
} nv_devices[64];
static unsigned nv_next_fd=1;
static unsigned nv_device_index(unsigned session,unsigned fd) {
 for(unsigned i=0;i<64;i++) if(nv_devices[i].open && nv_devices[i].fd==fd && fd && nv_devices[i].owner==session+1) return i;
 boundary("nv-device-descriptor",&state,state.pc);
}
static void nv_release_device(unsigned i) {
 if(!nv_devices[i].refs) boundary("nv-device-reference-invariant",&state,state.pc);
 if(--nv_devices[i].refs) return;
 unsigned space=nv_devices[i].channel_space,map=nv_devices[i].channel_nvmap;
 if(nv_devices[i].kind==NV_DEVICE_CHANNEL) gpu_close_channel(&nv_devices[i].channel);
 if(nv_devices[i].kind==NV_DEVICE_AS_GPU) gpu_close_space(&nv_devices[i].address_space);
 if(nv_devices[i].kind==NV_DEVICE_MAP) nvmap_close_client(i);
 for(unsigned j=0;j<3;j++) if(nv_devices[i].events[j] && !CloseHandle(nv_devices[i].events[j])) boundary("nv-event-close",&state,state.pc);
 memset(&nv_devices[i],0,sizeof(nv_devices[i]));
 if(space) nv_release_device(space-1);
 if(map) nv_release_device(map-1);
}
static void nv_close_device(unsigned i) {
 if(!nv_devices[i].open) boundary("nv-device-close-invariant",&state,state.pc);
 nv_devices[i].open=0;nv_release_device(i);
}
static void nv_release_devices(unsigned session) {
 for(unsigned i=0;i<64;i++) if(nv_devices[i].open && nv_devices[i].owner==session+1) nv_close_device(i);
}
/* Channel edges are acyclic: channel -> address space / nvmap client. */
static void nv_bind_channel(unsigned device,uint8_t *buffer,const IpcRequest *request,int map) {
 if(request->send_size!=4 || request->receive_size) boundary("nv-channel-bind-layout",&state,state.pc);
 Region source=query(request->send_address);
 if(!(source.permission&1) || 4>source.size-(request->send_address-source.base)) boundary("nv-channel-bind-memory",&state,state.pc);
 unsigned fd=ipc_u32((void*)(uintptr_t)request->send_address),other=64;
 for(unsigned i=0;i<64;i++) if(nv_devices[i].open && nv_devices[i].owner==nv_devices[device].owner && nv_devices[i].fd==fd) other=i;
 unsigned channel=map ? device : other,target=map ? other : device;
 if(other==64 || nv_devices[other].kind!=(map ? NV_DEVICE_MAP : NV_DEVICE_CHANNEL)) {ipc_reply_u32(buffer,256,4,request->token);return;}
 if(!map && !nv_devices[target].address_space.initialized) {ipc_reply_u32(buffer,256,8,request->token);return;}
 unsigned *binding=map ? &nv_devices[channel].channel_nvmap : &nv_devices[channel].channel_space;
 if(*binding) {ipc_reply_u32(buffer,256,8,request->token);return;}
 nv_devices[target].refs++;*binding=target+1;
 ipc_reply_u32(buffer,256,0,request->token);
}
static void nv_dispatch(unsigned session,uint8_t *buffer,const IpcRequest *request) {
 if(request->domain || request->input_object_count || (request->receive_count && request->command!=1 && request->command!=11)) boundary("nv-request-layout",&state,state.pc);
 if(request->command==3) {
  if(sessions[session].nv_transfer || request->copy_count!=2 || request->copy_handle!=0xffff8001 ||
     request->argument_size<4 || request->send_size) boundary("nv-initialize-layout",&state,state.pc);
  unsigned slot=transfer_index(request->copy_handle2);
  uint32_t size=ipc_u32(request->arguments);
  if(!size || size!=transfer_memory[slot].size) boundary("nv-transfer-size",&state,state.pc);
  sessions[session].nv_transfer=transfer_retain(request->copy_handle2);
  printf("NV initialized transfer=%u size=0x%x\n",sessions[session].nv_transfer,size);
  ipc_reply_u32(buffer,256,0,request->token);return;
 }
 if(!sessions[session].nv_transfer) boundary("nv-not-initialized",&state,state.pc);
 if(request->copy_count) boundary("nv-unexpected-handle",&state,state.pc);
 if(request->command==11) {
  if(request->argument_size<8 || request->send_pid || request->send_count!=2) boundary("nv-ioctl2-layout",&state,state.pc);
  unsigned i=nv_device_index(session,ipc_u32(request->arguments)),code=ipc_u32(request->arguments+4);
  printf("NV ioctl2 fd=%u kind=%u request=0x%08x input=%llu inline=%llu output=%llu\n",nv_devices[i].fd,nv_devices[i].kind,code,(unsigned long long)request->send_size,(unsigned long long)request->send_size2,(unsigned long long)request->receive_size);
  if(nv_devices[i].kind!=NV_DEVICE_CHANNEL || code!=0xc018481b) boundary("nv-ioctl2",&state,state.pc);
  if(request->send_size!=24 || request->receive_count!=1 || request->receive_size<24) boundary("nv-submit-layout",&state,state.pc);
  Region source=query(request->send_address),extra=query(request->send_address2),target=query(request->receive_address);
  if(!(source.permission&1) || 24>source.size-(request->send_address-source.base) ||
     !(extra.permission&1) || request->send_size2>extra.size-(request->send_address2-extra.base) ||
     !(target.permission&2) || 24>target.size-(request->receive_address-target.base)) boundary("nv-submit-memory",&state,state.pc);
  uint32_t words[6];memcpy(words,(void*)(uintptr_t)request->send_address,24);
  if(!words[2] || words[2]>nv_devices[i].channel.capacity || request->send_size2!=(uint64_t)words[2]*8) boundary("nv-submit-count",&state,state.pc);
  printf("GPU submit count=%u flags=0x%x fence=%u:%u\n",words[2],words[3],words[4],words[5]);
  GpuChannel *channel=&nv_devices[i].channel;
  const char *submit_error=gpu_check_submission(channel,words[2],words[3],words[5]);
  if(submit_error) {printf("GPU submit rejected: %s\n",submit_error);boundary("nv-submit-state",&state,state.pc);}
  GpuPacketState packets={0};GpuPacketAudit audit={.engine=&nv_devices[i].channel.engine};unsigned total_words=0;
  unsigned bound_space=nv_devices[i].channel_space;
  GpuAddressSpace *space=bound_space ? &nv_devices[bound_space-1].address_space : NULL;
  uint64_t *descriptors=malloc((size_t)words[2]*8);
  if(!descriptors) boundary("gpu-submit-allocation",&state,state.pc);
  memcpy(descriptors,(void*)(uintptr_t)request->send_address2,(size_t)words[2]*8);
  unsigned preflight_words=0;
  for(unsigned j=0;j<words[2];j++) {
   const char *error=gpu_check_descriptor(space,descriptors[j],&preflight_words);
   if(error) {free(descriptors);boundary(error,&state,state.pc);}
  }
  unsigned first=channel->write_index;
  gpu_begin_submission(channel,descriptors,words[2],words[3],words[5]);free(descriptors);
  audit.engine->write_word=gpu_write_mapped_word;
  audit.engine->check_write=gpu_check_mapped_write;
  audit.engine->macro_failure_report=gpu_audit_macro_failure;
  audit.engine->indexed_draw_failure_report=gpu_capture_index_window;
  audit.engine->indexed_draw=GetEnvironmentVariableW(L"AC_EXPERIMENTAL_DRAW_PACKAGE",NULL,0) ? gpu_experimental_draw : NULL;
  audit.engine->conservative_zcull=1; /* No host hierarchical-depth cache. */
  audit.engine->decompress_surface=gpu_decompress_canonical_surface;
  audit.engine->clear_color=gpu_clear_canonical_color;
  audit.engine->memory_context=bound_space ? &nv_devices[bound_space-1].address_space : NULL;
  for(unsigned j=0;j<words[2];j++) {
   uint64_t entry=channel->entries[(first+j)%channel->capacity];
   unsigned high=entry>>32,count=(high>>10)&0x1fffff;
   uint64_t address=(entry&0xfffffffcull)|((uint64_t)(high&255)<<32);
   printf("GPU descriptor[%u]=0x%016llx address=0x%llx words=%u fetch=%u priv=%u level=%u sync=%u\n",j,(unsigned long long)entry,(unsigned long long)address,count,(unsigned)entry&1,(high>>8)&1,(high>>9)&1,high>>31);
   if(count) {
    GpuMemoryView view=gpu_resolve(space,address,(uint64_t)count*4);
    if(!view.bytes) boundary("gpu-pushbuffer-memory",&state,state.pc);
    if(view.kind || (entry&3) || (high&0x100)) boundary("gpu-pushbuffer-options",&state,state.pc);
    if(count>65536-total_words) boundary("gpu-pushbuffer-decode-budget",&state,state.pc);
    total_words+=count;
    for(unsigned k=0;k<count;k++) {
     const char *error=gpu_packet_word(&packets,ipc_u32((uint8_t*)view.bytes+k*4),gpu_audit_method,&audit);
     if(error) {printf("GPU packet error descriptor=%u word=%u reason=%s\n",j,k,error);boundary("gpu-packet-layout",&state,state.pc);}
     if(!(high&0x200) && !audit.execution_error) channel->top_level_get=address+(uint64_t)(k+1)*4;
    }
   }
   if(!audit.execution_error) gpu_consume_descriptor(channel);
  }
  printf("GPU decoded words=%u method_writes=%u pending=%u\n",total_words,audit.writes,packets.remaining);
  if(packets.remaining) boundary("gpu-packet-incomplete",&state,state.pc);
  printf("GPU engine prefix=%u MME code_writes=%u entry_writes=%u\n",audit.executed,audit.engine->code_writes,audit.engine->start_writes);
  printf("GPU constant-buffer uploaded_words=%llu\n",(unsigned long long)audit.engine->constant_buffer_words);
  printf("GPU inline-DMA words=%llu completed=%llu remaining=%llu\n",(unsigned long long)audit.engine->inline_dma.words,
   (unsigned long long)audit.engine->inline_dma.completed,(unsigned long long)audit.engine->inline_dma.remaining);
  printf("GPU syncpoint=%u completed=%u report_semaphore_releases=%llu\n",nv_devices[i].channel.syncpoint,
   gpu_syncpoints[nv_devices[i].channel.syncpoint].completed,(unsigned long long)audit.engine->report_semaphore_releases);
  printf("GPU color_clears=%llu depth_stencil_clears=%llu surface_decompressions=%llu\n",(unsigned long long)audit.engine->color_clears,(unsigned long long)audit.engine->depth_stencil_clears,(unsigned long long)audit.engine->surface_decompressions);
  printf("GPU indexed_draws=%llu pixel_shader_barriers=%llu system_pixel_barriers=%llu texture_wait_invalidations=%llu\n",
   (unsigned long long)audit.engine->indexed_draws,(unsigned long long)audit.engine->pixel_shader_barriers[0],
   (unsigned long long)audit.engine->pixel_shader_barriers[1],(unsigned long long)audit.engine->texture_wait_invalidations);
  if(audit.execution_error) boundary("gpu-engine-method",&state,state.pc);
  if(audit.engine->macro_active) boundary("gpu-macro-parameter-incomplete",&state,state.pc);
  if(audit.engine->inline_dma.active) boundary("gpu-inline-dma-incomplete",&state,state.pc);
  if(channel->pending || audit.engine->pending_work ||
     gpu_syncpoints[channel->syncpoint].completed!=gpu_syncpoints[channel->syncpoint].submitted)
   boundary("gpu-submission-incomplete",&state,state.pc);
  words[3]=0;words[4]=channel->syncpoint;words[5]=gpu_syncpoints[channel->syncpoint].submitted;
  memcpy((void*)(uintptr_t)request->receive_address,words,24);
  printf("GPU submission complete fence=%u:%u get=%u put=%u\n",words[4],words[5],channel->read_index,channel->write_index);
  ipc_reply_u32(buffer,256,0,request->token);return;
 }
 if(request->command==13) {
  if(request->argument_size<8 || request->send_size || request->send_pid) boundary("nv-margin-layout",&state,state.pc);
  memcpy(&sessions[session].nv_margin_requested,request->arguments,8);
  /* Host policy disables the console graphics-firmware carveout. The request
   * is retained, but cannot enable a margin without that system setting. */
  printf("NV firmware margin requested=0x%llx configured=disabled\n",(unsigned long long)sessions[session].nv_margin_requested);
  reply_result(buffer,request,0);return;
 }
 if(request->command==8) {
  uint64_t aruid=0;
  if(request->argument_size<8 || !request->send_pid || request->send_size) boundary("nv-client-layout",&state,state.pc);
  memcpy(&aruid,request->arguments,8);
  if(aruid!=GetCurrentProcessId()) boundary("nv-client-identity",&state,state.pc);
  sessions[session].nv_aruid=aruid;ipc_reply_u32(buffer,256,0,request->token);return;
 }
 if(request->command==0) {
  if(request->send_pid) boundary("nv-device-open-layout",&state,state.pc);
  Region r=query(request->send_address);
  if(!request->send_size || request->send_size>64 || !(r.permission&1) || request->send_size>r.size-(request->send_address-r.base))
   boundary("nv-device-path-memory",&state,state.pc);
  const uint8_t *path=(const uint8_t*)(uintptr_t)request->send_address;
  printf("NV device requested: ");
  for(unsigned i=0;i<request->send_size && path[i];i++) putchar(path[i]>=32 && path[i]<127 ? path[i] : '?');
  putchar('\n');
  size_t length=0;while(length<request->send_size && path[length]) length++;
  for(size_t j=length;j<request->send_size;j++) if(path[j]) boundary("nv-device-path-tail",&state,state.pc);
  const char *names[]={"/dev/nvhost-ctrl-gpu","/dev/nvhost-as-gpu","/dev/nvmap","/dev/nvhost-gpu"};
  unsigned kind=0;for(unsigned j=0;j<4;j++) if(length==strlen(names[j]) && !memcmp(path,names[j],length)) kind=j+1;
  if(!kind) boundary("nv-device-open",&state,state.pc);
  unsigned i=0;while(i<64 && nv_devices[i].kind) i++;
  if(i==64 || nv_next_fd>=0x7fffffff) boundary("nv-device-limit",&state,state.pc);
  nv_devices[i].fd=nv_next_fd++;nv_devices[i].owner=session+1;nv_devices[i].kind=kind;
  nv_devices[i].open=nv_devices[i].refs=1;
  /* Open returns {u32 fd,u32 driver_result}, separate from the CMIF result. */
  ipc_reply_u64(buffer,256,nv_devices[i].fd,request->token);return;
 }
 if(request->command==2) {
  if(request->argument_size<4 || request->send_size || request->send_pid) boundary("nv-device-close-layout",&state,state.pc);
  unsigned i=nv_device_index(session,ipc_u32(request->arguments));
  nv_close_device(i);ipc_reply_u32(buffer,256,0,request->token);return;
 }
 if(request->command==4) {
  if(request->argument_size<8 || request->send_pid || request->send_size) boundary("nv-event-layout",&state,state.pc);
  unsigned i=nv_device_index(session,ipc_u32(request->arguments)),id=ipc_u32(request->arguments+4);
  printf("NV event fd=%u id=%u\n",nv_devices[i].fd,id);
  unsigned limit=nv_devices[i].kind==NV_DEVICE_CTRL_GPU ? 2 : nv_devices[i].kind==NV_DEVICE_CHANNEL ? 3 : 0;
  if(id<1 || id>limit) boundary("nv-event-selector",&state,state.pc);
  /* Channel selectors 1/2 are SM breakpoint reports; 3 is error notification.
   * They stay unsignaled until an actual corresponding condition occurs. */
  /* Control notifications start unsignaled. No GPU work or error is invented.
   * Guest copies retain the event after the owning descriptor is closed. */
  unsigned handle=copy_event(&nv_devices[i].events[id-1]);
  memset(buffer,0,52);ipc_reply_copy(buffer,256,handle,0,request->token);
  ipc_put32(buffer+4,0x80000009);ipc_put32(buffer+32,0);return;
 }
 if(request->command==1) {
  if(request->argument_size<8 || request->send_pid) boundary("nv-ioctl-layout",&state,state.pc);
  unsigned i=nv_device_index(session,ipc_u32(request->arguments));
  printf("NV ioctl fd=%u kind=%u request=0x%08x input=%llu output=%llu\n",nv_devices[i].fd,nv_devices[i].kind,
   ipc_u32(request->arguments+4),(unsigned long long)request->send_size,(unsigned long long)request->receive_size);
  if(nv_devices[i].kind==NV_DEVICE_MAP) {nvmap_ioctl(i,ipc_u32(request->arguments+4),buffer,request);return;}
  if(nv_devices[i].kind==NV_DEVICE_AS_GPU && ipc_u32(request->arguments+4)==0x40044101) {
   nv_bind_channel(i,buffer,request,0);return;
  }
  if(nv_devices[i].kind==NV_DEVICE_CHANNEL && ipc_u32(request->arguments+4)==0x40044801) {
   nv_bind_channel(i,buffer,request,1);return;
  }
  if(nv_devices[i].kind==NV_DEVICE_CHANNEL && ipc_u32(request->arguments+4)==0xc020481a) {
   nv_allocate_fifo(&nv_devices[i].channel,nv_devices[i].fd,nv_devices[i].channel_space && nv_devices[i].channel_nvmap,buffer,request);return;
  }
  if(nv_devices[i].kind==NV_DEVICE_CHANNEL && ipc_u32(request->arguments+4)==0xc0104809) {
   nv_allocate_context(&nv_devices[i].channel,buffer,request);return;
  }
  if(nv_devices[i].kind==NV_DEVICE_CHANNEL && (ipc_u32(request->arguments+4)==0x40044803 || ipc_u32(request->arguments+4)==0x4004480d)) {
   nv_configure_channel(&nv_devices[i].channel,buffer,request,ipc_u32(request->arguments+4)==0x4004480d);return;
  }
  if(nv_devices[i].kind==NV_DEVICE_CHANNEL && ipc_u32(request->arguments+4)==0xc018480c) {
   nv_configure_error_notifier(&nv_devices[i].channel,nv_devices[i].channel_nvmap,buffer,request);return;
  }
  if(nv_devices[i].kind==NV_DEVICE_CHANNEL && ipc_u32(request->arguments+4)==0x40084714) {
   nv_set_channel_user_data(&nv_devices[i].channel,buffer,request);return;
  }
  if(nv_devices[i].kind==NV_DEVICE_CHANNEL && ipc_u32(request->arguments+4)==0xc004481d) {
   nv_set_channel_timeslice(&nv_devices[i].channel,buffer,request);return;
  }
  if(nv_devices[i].kind==NV_DEVICE_CHANNEL && ipc_u32(request->arguments+4)==0xc010480b) {
   unsigned space=nv_devices[i].channel_space;
   nv_bind_zcull(&nv_devices[i].channel,space ? &nv_devices[space-1].address_space : NULL,buffer,request);return;
  }
  if(nv_devices[i].kind==NV_DEVICE_AS_GPU && (ipc_u32(request->arguments+4)==0xc0284106 || ipc_u32(request->arguments+4)==0xc0084105)) {
   gpu_mapping_ioctl(&nv_devices[i].address_space,buffer,request,ipc_u32(request->arguments+4)==0xc0084105);return;
  }
  if(nv_devices[i].kind==NV_DEVICE_AS_GPU && ipc_u32(request->arguments+4)==0x40284109) {
   gpu_as_initialize(&nv_devices[i].address_space,buffer,request);return;
  }
  if(nv_devices[i].kind==NV_DEVICE_AS_GPU && ipc_u32(request->arguments+4)==0xc0404108) {
   gpu_as_get_regions(&nv_devices[i].address_space,buffer,request);return;
  }
  if(nv_devices[i].kind==NV_DEVICE_AS_GPU && (ipc_u32(request->arguments+4)==0xc0184102 || ipc_u32(request->arguments+4)==0xc0104103)) {
   gpu_as_reservation_ioctl(&nv_devices[i].address_space,buffer,request,ipc_u32(request->arguments+4)==0xc0104103);return;
  }
  if(nv_devices[i].kind==NV_DEVICE_CTRL_GPU && ipc_u32(request->arguments+4)==0xc0b04705) {
   nv_get_characteristics(buffer,request);return;
  }
  if(nv_devices[i].kind==NV_DEVICE_CTRL_GPU && ipc_u32(request->arguments+4)==0xc0184706) {
   nv_get_tpc_masks(buffer,request);return;
  }
  if(nv_devices[i].kind==NV_DEVICE_CTRL_GPU && ipc_u32(request->arguments+4)==0x80084714) {
   nv_get_zbc_slots(buffer,request);return;
  }
  if(nv_devices[i].kind==NV_DEVICE_CTRL_GPU && ipc_u32(request->arguments+4)==0x402c4703) {
   nv_set_zbc(buffer,request);return;
  }
  if(nv_devices[i].kind==NV_DEVICE_CTRL_GPU && (ipc_u32(request->arguments+4)==0x80044701 || ipc_u32(request->arguments+4)==0x80284702)) {
   nv_get_zcull(buffer,request,ipc_u32(request->arguments+4)==0x80284702);return;
  }
  boundary("nv-ioctl",&state,state.pc);
 }
 boundary("nv-command",&state,state.pc);
}
