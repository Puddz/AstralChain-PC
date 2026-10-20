/* Nvmap objects and client references are distinct from device descriptors.
 * Creation reserves an object ID; backing memory is attached by Alloc. */
static unsigned nvmap_next_id=4;
static unsigned nvmap_find(unsigned id) {
 for(unsigned i=0;i<256;i++) if(nvmap_objects[i].id==id && id) return i;
 return 256;
}
static void nvmap_collect(unsigned i) {
 if(nvmap_objects[i].mapped_refs || nvmap_objects[i].display_refs) return;
 for(unsigned j=0;j<64;j++) if(nvmap_objects[i].refs[j]) return;
 memset(&nvmap_objects[i],0,sizeof(nvmap_objects[i]));
}
static void nvmap_close_client(unsigned client) {
 for(unsigned i=0;i<256;i++) if(nvmap_objects[i].id) {nvmap_objects[i].refs[client]=0;nvmap_collect(i);}
}
static void nvmap_ioctl(unsigned client,unsigned code,uint8_t *buffer,const IpcRequest *request) {
 unsigned size=code==0xc0080101 || code==0xc0080103 || code==0xc008010e ? 8 : code==0xc0180105 ? 24 : code==0xc00c0109 ? 12 : code==0xc0200104 ? 32 : 0;
 if(!size) boundary("nvmap-ioctl",&state,state.pc);
 if(request->send_size!=size || request->receive_count!=1 || request->receive_size<size) boundary("nvmap-layout",&state,state.pc);
 Region source=query(request->send_address),target=query(request->receive_address);
 if(!(source.permission&1) || size>source.size-(request->send_address-source.base) ||
    !(target.permission&2) || size>target.size-(request->receive_address-target.base)) boundary("nvmap-memory",&state,state.pc);
 uint8_t data[32];memcpy(data,(void*)(uintptr_t)request->send_address,size);unsigned result=0;
 if(code==0xc0080101) {
  unsigned bytes=ipc_u32(data),i=0;while(i<256 && nvmap_objects[i].id) i++;
  if(!bytes || bytes>0xfffff000u) result=4;
  else if(i==256 || nvmap_next_id>=0x7ffffffc) result=6;
  else {
   nvmap_objects[i].id=nvmap_next_id;nvmap_next_id+=4;
   nvmap_objects[i].size=(bytes+4095u)&~4095u;nvmap_objects[i].refs[client]=1;
   ipc_put32(data+4,nvmap_objects[i].id);
   printf("NVMAP create id=%u requested=0x%x size=0x%x\n",nvmap_objects[i].id,bytes,nvmap_objects[i].size);
  }
 } else {
  unsigned id=ipc_u32(data+(code==0xc008010e ? 4 : 0)),i=nvmap_find(id);
  if(i==256 || (code!=0xc0080103 && !nvmap_objects[i].refs[client])) result=4;
  else if(code==0xc0080103) {
   if(nvmap_objects[i].refs[client]==65535) result=6;
   else {nvmap_objects[i].refs[client]++;ipc_put32(data+4,id);}
  } else if(code==0xc008010e) ipc_put32(data,id);
  else if(code==0xc0180105) {
   unsigned bytes=nvmap_objects[i].size,uncached=(nvmap_objects[i].flags>>1)&1;
   uint64_t address=nvmap_objects[i].address;nvmap_objects[i].refs[client]--;nvmap_collect(i);
   if(nvmap_objects[i].id) address=0;
   memcpy(data+8,&address,8);ipc_put32(data+16,bytes);ipc_put32(data+20,uncached);
  } else if(code==0xc00c0109) {
   unsigned param=ipc_u32(data+4);
   if(param==1) ipc_put32(data+8,nvmap_objects[i].size);
   else if(param==2) ipc_put32(data+8,nvmap_objects[i].alignment);
   else if(param==3) result=4;
   else if(param==4) ipc_put32(data+8,0x40000000);
   else if(param==5) ipc_put32(data+8,nvmap_objects[i].kind);
   else boundary("nvmap-param",&state,state.pc);
  } else {
   uint64_t address;memcpy(&address,data+24,8);
   printf("NVMAP alloc id=%u heap=0x%x flags=0x%x align=0x%x kind=0x%x address=0x%llx\n",id,ipc_u32(data+4),ipc_u32(data+8),ipc_u32(data+12),data[16],(unsigned long long)address);
   unsigned heap=ipc_u32(data+4),flags=ipc_u32(data+8),alignment=ipc_u32(data+12);
   if(heap && heap!=0x40000000) boundary("nvmap-heap",&state,state.pc);
   if(flags&~3u) boundary("nvmap-flags",&state,state.pc);
   if(!address) boundary("nvmap-driver-backing",&state,state.pc);
   Region backing=query(address);
   if(nvmap_objects[i].allocated) result=13;
   else if((alignment && (alignment&(alignment-1))) || (address&4095) ||
           address<HEAP_BASE || address>=HEAP_BASE+heap_size || backing.kind!=5 || backing.permission!=3 ||
           nvmap_objects[i].size>backing.size-(address-backing.base)) result=4;
   else {
    if(alignment<4096) alignment=4096;
    nvmap_objects[i].address=address;nvmap_objects[i].alignment=alignment;
    nvmap_objects[i].flags=flags;nvmap_objects[i].kind=data[16];nvmap_objects[i].allocated=1;
    nvmap_objects[i].storage=GPU_STORAGE_CANONICAL;
    ipc_put32(data+12,alignment);
   }
  }
 }
 if(!result) memcpy((void*)(uintptr_t)request->receive_address,data,size);
 ipc_reply_u32(buffer,256,result,request->token);
}
