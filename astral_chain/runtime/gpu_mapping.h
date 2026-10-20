/* Ordered kind runs cover the actual mapping, never reservation padding.
 * Limits are bounded implementation capacity, not hardware limits. */
enum { GPU_KIND_SPANS=512 };
static int gpu_supported_mapping_kind(unsigned kind) {
 return kind==0 || kind==0xfe || kind==0xdb || kind==0x51 || kind==0x86 || kind==0xe9;
}
static int gpu_append_kind(GpuKindSpan *runs,unsigned *count,uint64_t end,unsigned kind) {
 if(*count && runs[*count-1].kind==kind) {runs[*count-1].end=end;return 1;}
 if(*count==GPU_KIND_SPANS) return 0;
 runs[(*count)++]=(GpuKindSpan){end,kind};return 1;
}
static unsigned gpu_modify_kind(GpuAddressSpace *space,unsigned kind,uint64_t address,uint64_t offset,uint64_t size) {
 if(!gpu_supported_mapping_kind(kind)) boundary("gpu-modify-kind",&state,state.pc);
 for(unsigned i=0;i<GPU_AS_SLOTS;i++) if(space->mappings[i].size && address>=space->mappings[i].address) {
  uint64_t relative=address-space->mappings[i].address,extent=space->mappings[i].size;
  if(relative>=extent) continue;
  unsigned page=space->mappings[i].page;
  if(!size || address%page || offset%page || size%page || offset>=extent-relative || size>extent-relative-offset || space->mappings[i].bound_refs) return 4;
  uint64_t begin=relative+offset,finish=begin+size,start=0;
  GpuKindSpan *runs=malloc(GPU_KIND_SPANS*sizeof(*runs));if(!runs) return 6;
  unsigned count=0,old_count=space->mappings[i].kind_count;
  for(unsigned j=0;j<(old_count ? old_count : 1);j++) {
   uint64_t end=old_count ? space->mappings[i].kinds[j].end : extent;
   unsigned old_kind=old_count ? space->mappings[i].kinds[j].kind : space->mappings[i].kind;
   int ok=1;
   if(start<begin) ok=gpu_append_kind(runs,&count,end<begin ? end : begin,old_kind);
   uint64_t middle_start=start>begin ? start : begin,middle_end=end<finish ? end : finish;
   if(ok && middle_start<middle_end) ok=gpu_append_kind(runs,&count,middle_end,kind);
   if(ok && end>finish) ok=gpu_append_kind(runs,&count,end,old_kind);
   if(!ok) {free(runs);return 6;}
   start=end;
  }
  /* Publish only after all validation/allocation succeeds. No bytes, mapping
   * ownership, reservations or other aliases change with a kind remap. */
  free(space->mappings[i].kinds);
  if(count==1) {space->mappings[i].kind=runs[0].kind;free(runs);runs=NULL;count=0;}
  space->mappings[i].kinds=runs;space->mappings[i].kind_count=count;return 0;
 }
 return 4;
}
/* Software GPU address translation over retained CPU bytes. Reservation padding
 * is deliberately not backed. No physical GPU allocation or submission here. */
static unsigned gpu_map_buffer(GpuAddressSpace *space,unsigned flags,unsigned kind,unsigned id,unsigned page,
 uint64_t buffer_offset,uint64_t size,uint64_t *offset) {
 printf("GPU map id=%u flags=0x%x kind=0x%x page=0x%x buffer_offset=0x%llx size=0x%llx offset=0x%llx\n",id,flags,kind,page,
  (unsigned long long)buffer_offset,(unsigned long long)size,(unsigned long long)*offset);
 if(!space->initialized) return 8;
 if(flags==0x100) {
  if(id || page) return 4;
  return gpu_modify_kind(space,kind,*offset,buffer_offset,size);
 }
 if(flags&~5u) boundary("gpu-map-flags",&state,state.pc);
 if(!(flags&1) && *offset) boundary("gpu-map-input-offset",&state,state.pc);
 unsigned object=nvmap_find(id);
 if(object==256 || !nvmap_objects[object].allocated) return 4;
 unsigned owned=0;for(unsigned j=0;j<64;j++) owned|=nvmap_objects[object].refs[j];
 if(!owned) return 4;
 if(kind==0xffffffffu) kind=nvmap_objects[object].kind;
 /* Retain storage aliases, not decoded pixels. Libnx identifies 0xdb as
  * C32_2CRA; deko3d creates this alongside pitch and generic image aliases.
  * Mapping itself does not compress or initialize storage. Rendering/image
  * consumers must implement the requested layout and compression semantics;
  * Libnx identifies 0x51 as Z24S8_2CZ; deko3d selects it for compressed
  * single-sample Z24S8 depth/stencil storage, and 0x86 as ZF32_2CZ for
  * compressed single-sample float depth. 0xe9 is C64_2CRA, selected for
  * compressed single-sample 64-bit color storage. Retain those kinds without
  * treating compressed depth bytes as decoded pixels. Existing pitch-only
  * writers and command fetches reject all non-pitch kinds. */
 if(!gpu_supported_mapping_kind(kind)) boundary("gpu-map-kind",&state,state.pc);
 if(!page) page=4096;
 if(page!=4096 && page!=space->big_page_size) return 4;
 uint64_t backing=nvmap_objects[object].size;
 if(buffer_offset>=backing || buffer_offset%4096) return 4;
 if(!size) size=backing-buffer_offset;
 if(size>backing-buffer_offset || size%4096) return 4;
 uint64_t span=(size+page-1)&~((uint64_t)page-1);
 unsigned slot=0;while(slot<GPU_AS_SLOTS && space->mappings[slot].size) slot++;
 if(slot==GPU_AS_SLOTS) return 6;
 uint64_t address=*offset;
 if(flags&1) {
  if(address%page) return 4;
  unsigned contained=0;
  for(unsigned i=0;i<GPU_AS_SLOTS;i++) {
   uint64_t base=space->reservations[i].address,length=space->reservations[i].size;
   if(length && space->reservations[i].page_size==page && address>=base && address-base<length && span<=length-(address-base)) contained=1;
  }
  if(!contained) return 4;
  for(unsigned i=0;i<GPU_AS_SLOTS;i++) if(space->mappings[i].size && address<space->mappings[i].address+space->mappings[i].span && space->mappings[i].address<address+span) return 4;
 } else {
  unsigned result=gpu_as_reserve(space,(unsigned)(span/page),page,0,&address);
  if(result) return result;
 }
 space->mappings[slot].address=address;space->mappings[slot].size=size;space->mappings[slot].span=span;
 space->mappings[slot].cpu=nvmap_objects[object].address+buffer_offset;
 space->mappings[slot].object=object;space->mappings[slot].page=page;
 space->mappings[slot].kind=kind;space->mappings[slot].flags=flags;
 nvmap_objects[object].mapped_refs++;*offset=address;return 0;
}
typedef struct { void *bytes; unsigned kind,flags,storage; } GpuMemoryView;
static GpuMemoryView gpu_resolve(GpuAddressSpace *space,uint64_t address,uint64_t size) {
 if(!size) return (GpuMemoryView){0};
 for(unsigned i=0;i<GPU_AS_SLOTS;i++) if(space->mappings[i].size && address>=space->mappings[i].address) {
  uint64_t relative=address-space->mappings[i].address;
  if(relative<space->mappings[i].size && size<=space->mappings[i].size-relative) {
   unsigned kind=space->mappings[i].kind;
   for(unsigned j=0;j<space->mappings[i].kind_count;j++) if(relative<space->mappings[i].kinds[j].end) {
    /* A raw view has one layout. A consumer must split mixed-kind accesses. */
    if(size>space->mappings[i].kinds[j].end-relative) return (GpuMemoryView){0};
    kind=space->mappings[i].kinds[j].kind;break;
   }
   return (GpuMemoryView){(void*)(uintptr_t)(space->mappings[i].cpu+relative),kind,space->mappings[i].flags,nvmap_objects[space->mappings[i].object].storage};
  }
 }
 return (GpuMemoryView){0};
}
/* Raw backing access only, not a decoded pixel or pitch-linear surface view. */
static void *gpu_translate(GpuAddressSpace *space,uint64_t address,uint64_t size) {
 return gpu_resolve(space,address,size).bytes;
}
static unsigned gpu_unmap_buffer(GpuAddressSpace *space,uint64_t address) {
 if(!space->initialized) return 8;
 for(unsigned i=0;i<GPU_AS_SLOTS;i++) if(space->mappings[i].size && space->mappings[i].address==address) {
  if(space->mappings[i].bound_refs) return 4;
  unsigned object=space->mappings[i].object,page=space->mappings[i].page,fixed=space->mappings[i].flags&1;
  uint64_t span=space->mappings[i].span;
  free(space->mappings[i].kinds);memset(&space->mappings[i],0,sizeof(space->mappings[i]));
  if(!fixed && gpu_as_free(space,address,(unsigned)(span/page),page)) boundary("gpu-map-reservation-invariant",&state,state.pc);
  if(!nvmap_objects[object].mapped_refs) boundary("gpu-map-reference-invariant",&state,state.pc);
  nvmap_objects[object].mapped_refs--;nvmap_collect(object);return 0;
 }
 return 4;
}
static void gpu_close_space(GpuAddressSpace *space) {
 for(unsigned i=0;i<GPU_AS_SLOTS;i++) if(space->mappings[i].size && gpu_unmap_buffer(space,space->mappings[i].address)) boundary("gpu-space-close-bound-mapping",&state,state.pc);
}
static void gpu_mapping_ioctl(GpuAddressSpace *space,uint8_t *buffer,const IpcRequest *request,int release) {
 unsigned size=release ? 8 : 40;
 if(request->send_size!=size || request->receive_count!=1 || request->receive_size<size) boundary("gpu-map-layout",&state,state.pc);
 Region source=query(request->send_address),target=query(request->receive_address);
 if(!(source.permission&1) || size>source.size-(request->send_address-source.base) ||
    !(target.permission&2) || size>target.size-(request->receive_address-target.base)) boundary("gpu-map-memory",&state,state.pc);
 uint8_t data[40];memcpy(data,(void*)(uintptr_t)request->send_address,size);
 uint64_t offset;unsigned result;
 if(release) {memcpy(&offset,data,8);result=gpu_unmap_buffer(space,offset);}
 else {
  uint64_t begin,length;memcpy(&begin,data+16,8);memcpy(&length,data+24,8);memcpy(&offset,data+32,8);
  result=gpu_map_buffer(space,ipc_u32(data),ipc_u32(data+4),ipc_u32(data+8),ipc_u32(data+12),begin,length,&offset);
  if(!result) {memcpy(data+32,&offset,8);if(ipc_u32(data)!=0x100 && !ipc_u32(data+12)) ipc_put32(data+12,4096);}
 }
 if(!result) memcpy((void*)(uintptr_t)request->receive_address,data,size);
 ipc_reply_u32(buffer,256,result,request->token);
}
