/* Owner-inaccessible transfer memory. A private host buffer holds the owned
 * bytes while the original heap pages are protected. On final close they are
 * restored. Host services retain object references independently of the guest
 * handle. Writable/readable owner aliases are unsupported. */
static uint32_t transfer_create(uint64_t address,uint64_t size,uint64_t permission) {
 if(permission) boundary("transfer-owner-permission",&state,state.pc);
 if(!size || size>64*1024*1024 || (address&4095) || (size&4095)) boundary("transfer-memory-extent",&state,state.pc);
 Region r=query(address);
 if(nvmap_overlaps(address,size)) boundary("transfer-nvmap-backing",&state,state.pc);
 if(uncached_overlap(address,size)) boundary("transfer-uncached-memory",&state,state.pc);
 if(r.kind!=5 || r.permission!=3 || size>r.size-(address-r.base)) boundary("transfer-memory-source",&state,state.pc);
 unsigned slot=32;uint64_t used=0;
 for(unsigned i=0;i<32;i++) {
  used+=transfer_memory[i].size;
  if(!transfer_memory[i].size && slot==32) slot=i;
 }
 if(slot==32 || size>256*1024*1024-used) boundary("transfer-memory-budget",&state,state.pc);
 void *data=VirtualAlloc(NULL,(SIZE_T)size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
 if(!data) return 0;
 memcpy(data,(void*)(uintptr_t)address,(size_t)size);
 DWORD previous;
 if(!(heap_gpu_pinned && address>=heap_base && address-heap_base<HEAP_LIMIT) && !VirtualProtect((void*)(uintptr_t)address,(SIZE_T)size,PAGE_NOACCESS,&previous)) {
  VirtualFree(data,0,MEM_RELEASE);boundary("transfer-memory-protect",&state,state.pc);
 }
 transfer_memory[slot].address=address;transfer_memory[slot].size=size;transfer_memory[slot].data=data;
 transfer_memory[slot].refs=1;transfer_memory[slot].handle_open=1;
 printf("TRANSFER created handle=0x%x address=0x%llx size=0x%llx\n",0x6000+slot,(unsigned long long)address,(unsigned long long)size);
 return 0x6000+slot;
}
static unsigned transfer_index(uint32_t handle) {
 if(handle<0x6000 || handle>=0x6020 || !transfer_memory[handle-0x6000].handle_open)
  boundary("transfer-memory-handle",&state,state.pc);
 return handle-0x6000;
}
static unsigned transfer_retain(uint32_t handle) {
 unsigned i=transfer_index(handle);transfer_memory[i].refs++;return i+1;
}
static void transfer_release(unsigned object) {
 if(!object || object>32 || !transfer_memory[object-1].refs) boundary("transfer-memory-reference",&state,state.pc);
 unsigned i=object-1;if(--transfer_memory[i].refs) return;
 DWORD previous;
 if(!VirtualProtect((void*)(uintptr_t)transfer_memory[i].address,(SIZE_T)transfer_memory[i].size,PAGE_READWRITE,&previous))
  boundary("transfer-memory-restore",&state,state.pc);
 memcpy((void*)(uintptr_t)transfer_memory[i].address,transfer_memory[i].data,(size_t)transfer_memory[i].size);
 if(!VirtualFree(transfer_memory[i].data,0,MEM_RELEASE)) boundary("transfer-memory-release",&state,state.pc);
 memset(&transfer_memory[i],0,sizeof(transfer_memory[i]));
}
static void transfer_close(uint32_t handle) {
 unsigned i=transfer_index(handle);transfer_memory[i].handle_open=0;transfer_release(i+1);
}
