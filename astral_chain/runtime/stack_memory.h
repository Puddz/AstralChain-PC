/* Heap-backed stack ownership transfer. The source is inaccessible while
 * mapped; writes are restored on unmap. This copy-based implementation needs
 * service serialization when guest thread scheduling is introduced. */
static void stack_map(uint64_t destination,uint64_t source,uint64_t size) {
 if(!size || size>1024*1024 || size%4096 || source%4096 || destination%4096 ||
    destination<STACK_BASE || destination>=STACK_BASE+0x100000000ull || size>STACK_BASE+0x100000000ull-destination)
  boundary("stack-map-extent",&state,state.pc);
 Region src=query(source),dst=query(destination);
 if(nvmap_overlaps(source,size)) boundary("stack-nvmap-backing",&state,state.pc);
 if(uncached_overlap(source,size)) boundary("stack-uncached-memory",&state,state.pc);
 if(src.kind!=5 || src.permission!=3 || size>src.size-(source-src.base)) boundary("stack-map-source",&state,state.pc);
 if(dst.kind || size>dst.size-(destination-dst.base)) boundary("stack-map-destination",&state,state.pc);
 unsigned slot=0;while(slot<64 && stack_maps[slot].size) slot++;
 if(slot==64) boundary("stack-map-limit",&state,state.pc);
 HANDLE backing=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,(DWORD)size,NULL);
 if(!backing) boundary("stack-backing-create",&state,state.pc);
 unsigned handle=copy_shared_object(backing,3,size);
 shared_map(handle,destination,size,3);
 memcpy((void*)(uintptr_t)destination,(void*)(uintptr_t)source,(size_t)size);
 DWORD previous;
 /* Pinned for GPU import (nvn_hle.h): CPU protection changes would fault the GPU. */
 if(!heap_gpu_pinned && !VirtualProtect((void*)(uintptr_t)source,(SIZE_T)size,PAGE_NOACCESS,&previous)) boundary("stack-source-protect",&state,state.pc);
 stack_maps[slot].source=source;stack_maps[slot].destination=destination;stack_maps[slot].size=size;
 stack_maps[slot].backing=backing;stack_maps[slot].handle=handle;
 printf("STACK mapped source=0x%llx destination=0x%llx size=0x%llx\n",(unsigned long long)source,(unsigned long long)destination,(unsigned long long)size);
}
static void stack_unmap(uint64_t destination,uint64_t source,uint64_t size) {
 unsigned slot=0;while(slot<64 && (!stack_maps[slot].size || stack_maps[slot].source!=source || stack_maps[slot].destination!=destination || stack_maps[slot].size!=size)) slot++;
 if(slot==64) boundary("stack-unmap-mismatch",&state,state.pc);
 DWORD previous;
 if(!VirtualProtect((void*)(uintptr_t)source,(SIZE_T)size,PAGE_READWRITE,&previous)) boundary("stack-source-restore",&state,state.pc);
 memcpy((void*)(uintptr_t)source,(void*)(uintptr_t)destination,(size_t)size);
 unsigned handle=stack_maps[slot].handle,index=handle-0x4000;
 shared_unmap(handle,destination,size);
 if(!CloseHandle(shared_handles[index]) || !CloseHandle(stack_maps[slot].backing)) boundary("stack-backing-close",&state,state.pc);
 shared_handles[index]=NULL;shared_sources[index]=NULL;shared_permissions[index]=0;shared_sizes[index]=0;
 memset(&stack_maps[slot],0,sizeof(stack_maps[slot]));
}
