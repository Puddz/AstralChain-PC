/* GPU virtual addresses are a separate namespace from CPU/Windows addresses. */
/* Bounded host capacity, not a hardware limit. Each image can have several aliases. */
enum { GPU_AS_SLOTS=1024 };
typedef struct {uint64_t end;unsigned kind;} GpuKindSpan;
typedef struct {
 unsigned initialized,flags,big_page_size;
 uint64_t start,split,end;
 struct { uint64_t address,size; unsigned page_size,flags; } reservations[GPU_AS_SLOTS];
 struct { uint64_t address,size,span,cpu; unsigned object,page,kind,flags,bound_refs;
  GpuKindSpan *kinds;unsigned kind_count; } mappings[GPU_AS_SLOTS];
} GpuAddressSpace;
static void gpu_as_initialize(GpuAddressSpace *space,uint8_t *buffer,const IpcRequest *request) {
 if(request->send_size!=40 || request->receive_size) boundary("nv-as-init-layout",&state,state.pc);
 Region source=query(request->send_address);
 if(!(source.permission&1) || 40>source.size-(request->send_address-source.base)) boundary("nv-as-input-memory",&state,state.pc);
 uint32_t words[10];memcpy(words,(void*)(uintptr_t)request->send_address,40);
 printf("NV AS init flags=%u big_page=0x%x\n",words[0],words[2]);
 if(space->initialized) {ipc_reply_u32(buffer,256,8,request->token);return;}
 if(words[0]>1 || words[1] || words[3]) boundary("nv-as-init-options",&state,state.pc);
 for(unsigned j=4;j<10;j++) if(words[j]) boundary("nv-as-custom-range",&state,state.pc);
 unsigned page=words[2] ? words[2] : gpu_model.big_page_size;
 if(page!=0x10000 && page!=0x20000) {ipc_reply_u32(buffer,256,4,request->token);return;}
 /* Two default user regions, ending at 128 GiB within the 40-bit GPU VA.
  * These limits will also constrain reservations and mappings. */
 *space=(GpuAddressSpace){1,words[0],page,(uint64_t)page<<16,1ull<<34,1ull<<37};
 ipc_reply_u32(buffer,256,0,request->token);
}
static unsigned gpu_as_reserve(GpuAddressSpace *space,unsigned pages,unsigned page,unsigned flags,uint64_t *offset) {
 if(!space->initialized) return 8;
 if(!pages || (page!=4096 && page!=space->big_page_size) || flags&~3u) return 4;
 uint64_t begin=page==4096 ? space->start : space->split,end=page==4096 ? space->split : space->end;
 uint64_t size=(uint64_t)pages*page,alignment=(flags&1) ? page : (*offset ? *offset : page);
 if(size>end-begin || alignment<page || alignment>end || (alignment&(alignment-1))) return 4;
 unsigned slot=0;while(slot<GPU_AS_SLOTS && space->reservations[slot].size) slot++;
 if(slot==GPU_AS_SLOTS) return 6;
 uint64_t candidate=(flags&1) ? *offset : (begin+alignment-1)&~(alignment-1);
 for(unsigned attempt=0;attempt<=GPU_AS_SLOTS;attempt++) {
  if(candidate<begin || candidate>end || size>end-candidate || candidate%page) return (flags&1) ? 4 : 6;
  uint64_t next=candidate;
  for(unsigned i=0;i<GPU_AS_SLOTS;i++) {
   uint64_t a=space->reservations[i].address,n=space->reservations[i].size;
   if(n && candidate<a+n && a<candidate+size && a+n>next) next=a+n;
  }
  if(next==candidate) {
   space->reservations[slot].address=candidate;space->reservations[slot].size=size;
   space->reservations[slot].page_size=page;space->reservations[slot].flags=flags;
   *offset=candidate;return 0;
  }
  if(flags&1) return 4;
  candidate=(next+alignment-1)&~(alignment-1);
 }
 return 6;
}
static unsigned gpu_as_free(GpuAddressSpace *space,uint64_t address,unsigned pages,unsigned page) {
 if(!space->initialized) return 8;
 if(!pages || (page!=4096 && page!=space->big_page_size)) return 4;
 uint64_t size=(uint64_t)pages*page;
 for(unsigned i=0;i<GPU_AS_SLOTS;i++) if(space->reservations[i].size==size && space->reservations[i].address==address && space->reservations[i].page_size==page) {
  for(unsigned j=0;j<GPU_AS_SLOTS;j++) if(space->mappings[j].size && space->mappings[j].address<address+size && address<space->mappings[j].address+space->mappings[j].span) return 4;
  memset(&space->reservations[i],0,sizeof(space->reservations[i]));return 0;
 }
 return 4;
}
static void gpu_as_reservation_ioctl(GpuAddressSpace *space,uint8_t *buffer,const IpcRequest *request,int release) {
 unsigned size=release ? 16 : 24;
 if(request->send_size!=size || request->receive_count!=1 || request->receive_size<size) boundary("nv-as-reservation-layout",&state,state.pc);
 Region source=query(request->send_address),target=query(request->receive_address);
 if(!(source.permission&1) || size>source.size-(request->send_address-source.base) ||
    !(target.permission&2) || size>target.size-(request->receive_address-target.base)) boundary("nv-as-reservation-memory",&state,state.pc);
 uint8_t data[24];memcpy(data,(void*)(uintptr_t)request->send_address,size);
 uint64_t offset;unsigned result;
 if(release) {
  memcpy(&offset,data,8);result=gpu_as_free(space,offset,ipc_u32(data+8),ipc_u32(data+12));
 } else {
  memcpy(&offset,data+16,8);
  printf("GPU reserve pages=0x%x page=0x%x flags=0x%x offset=0x%llx\n",ipc_u32(data),ipc_u32(data+4),ipc_u32(data+8),(unsigned long long)offset);
  result=gpu_as_reserve(space,ipc_u32(data),ipc_u32(data+4),ipc_u32(data+8),&offset);
  if(!result) memcpy(data+16,&offset,8);
 }
 if(!result) memcpy((void*)(uintptr_t)request->receive_address,data,size);
 ipc_reply_u32(buffer,256,result,request->token);
}
static void gpu_as_get_regions(GpuAddressSpace *space,uint8_t *buffer,const IpcRequest *request) {
 if(!space->initialized) boundary("nv-as-uninitialized",&state,state.pc);
 if(request->send_size!=64 || request->receive_count!=1 || request->receive_size<64) boundary("nv-as-regions-layout",&state,state.pc);
 Region source=query(request->send_address),target=query(request->receive_address);
 if(!(source.permission&1) || 64>source.size-(request->send_address-source.base) ||
    !(target.permission&2) || 64>target.size-(request->receive_address-target.base)) boundary("nv-as-regions-memory",&state,state.pc);
 uint8_t result[64];memcpy(result,(void*)(uintptr_t)request->send_address,64);
 unsigned capacity=ipc_u32(result+8)/24;if(capacity>2) capacity=2;
 for(unsigned j=0;j<capacity;j++) {
  uint64_t start=j ? space->split : space->start,end=j ? space->end : space->split;
  unsigned page=j ? space->big_page_size : 4096;uint64_t pages=(end-start)/page;
  memcpy(result+16+j*24,&start,8);ipc_put32(result+24+j*24,page);ipc_put32(result+28+j*24,0);memcpy(result+32+j*24,&pages,8);
 }
 ipc_put32(result+8,48);memcpy((void*)(uintptr_t)request->receive_address,result,64);
 ipc_reply_u32(buffer,256,0,request->token);
}
