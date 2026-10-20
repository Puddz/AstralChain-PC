/* Bounded 4 KiB..1 MiB shared objects. Clock/HID handles remain read-only;
 * internal stack backing permits read/write. Windows placeholders preserve
 * guest page addresses independently of 64 KiB host allocation granularity. */
static HANDLE shared_handles[64],shared_sources[64];
static unsigned shared_permissions[64];
static uint64_t shared_sizes[64];
static struct { uint64_t base; unsigned occupied; } shared_granules[64];
typedef PVOID (WINAPI *HostAlloc2)(HANDLE,PVOID,SIZE_T,ULONG,ULONG,MEM_EXTENDED_PARAMETER*,ULONG);
typedef PVOID (WINAPI *HostMap3)(HANDLE,HANDLE,PVOID,ULONG64,SIZE_T,ULONG,ULONG,MEM_EXTENDED_PARAMETER*,ULONG);
typedef BOOL (WINAPI *HostUnmap2)(HANDLE,PVOID,ULONG);
static HostAlloc2 shared_alloc2;
static HostMap3 shared_map3;
static HostUnmap2 shared_unmap2;
static uint32_t copy_shared_object(HANDLE source,unsigned permission,uint64_t size) {
 if(!size || size%4096 || size>1024*1024) boundary("shared-object-size",&state,state.pc);
 for(unsigned i=0;i<64;i++) if(!shared_handles[i]) {
  if(!DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&shared_handles[i],0,FALSE,DUPLICATE_SAME_ACCESS))
   boundary("host-shared-handle-copy",&state,state.pc);
  shared_sources[i]=source;shared_permissions[i]=permission;shared_sizes[i]=size;return 0x4000+i;
 }
 boundary("shared-handle-limit",&state,state.pc);
}
static uint32_t copy_shared_handle(HANDLE source,unsigned permission) { return copy_shared_object(source,permission,4096); }
static void shared_api(void) {
 if(shared_alloc2) return;
 HMODULE module=GetModuleHandleW(L"KernelBase.dll");
 shared_alloc2=(HostAlloc2)GetProcAddress(module,"VirtualAlloc2");
 shared_map3=(HostMap3)GetProcAddress(module,"MapViewOfFile3");
 shared_unmap2=(HostUnmap2)GetProcAddress(module,"UnmapViewOfFile2");
 if(!shared_alloc2 || !shared_map3 || !shared_unmap2) boundary("host-placeholder-api-unavailable",&state,state.pc);
}
static void shared_map(uint64_t handle,uint64_t address,uint64_t size,uint64_t permission) {
 if(handle<0x4000 || handle>=0x4040 || !shared_handles[handle-0x4000]) boundary("shared-map-handle",&state,state.pc);
 unsigned slot=handle-0x4000;
 if(!address || address%4096 || size!=shared_sizes[slot] || address>(1ull<<39)-size) boundary("shared-map-extent",&state,state.pc);
 if((permission!=1 && permission!=3) || shared_permissions[slot]!=permission) boundary("shared-map-permission",&state,state.pc);
 Region region=query(address);
 if(region.kind || size>region.size-(address-region.base)) boundary("shared-map-overlap",&state,state.pc);
 unsigned view=0;while(view<64 && shared_views[view].address) view++;
 if(view==64) boundary("shared-view-limit",&state,state.pc);
 shared_api();
 for(uint64_t offset=0;offset<size;offset+=4096) {
 uint64_t page_address=address+offset,base=page_address&~65535ull;
 unsigned granule=0;while(granule<64 && shared_granules[granule].base!=base) granule++;
 if(granule==64) {
  granule=0;while(granule<64 && shared_granules[granule].base) granule++;
  if(granule==64) boundary("shared-granule-limit",&state,state.pc);
  if(shared_alloc2(GetCurrentProcess(),(void*)(uintptr_t)base,65536,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0)!=(void*)(uintptr_t)base)
   boundary("host-shared-placeholder-reserve",&state,state.pc);
  shared_granules[granule].base=base;
  for(unsigned page=0;page<15;page++)
   if(!VirtualFree((void*)(uintptr_t)(base+page*4096),4096,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))
    boundary("host-shared-placeholder-split",&state,state.pc);
 }
 void *mapped=shared_map3(shared_handles[slot],GetCurrentProcess(),(void*)(uintptr_t)page_address,offset,4096,MEM_REPLACE_PLACEHOLDER,permission==1 ? PAGE_READONLY : PAGE_READWRITE,NULL,0);
 if(mapped!=(void*)(uintptr_t)page_address) boundary("host-shared-map",&state,state.pc);
 shared_granules[granule].occupied|=1u<<((page_address-base)/4096);
 }
 shared_views[view].address=address;shared_views[view].size=size;shared_views[view].source=shared_sources[slot];
 shared_views[view].permission=(unsigned)permission;
}
static void shared_unmap(uint64_t handle,uint64_t address,uint64_t size) {
 if(handle<0x4000 || handle>=0x4040 || !shared_handles[handle-0x4000]) boundary("shared-unmap-handle",&state,state.pc);
 if(size!=shared_sizes[handle-0x4000]) boundary("shared-unmap-size",&state,state.pc);
 unsigned view=0;while(view<64 && (shared_views[view].address!=address || shared_views[view].source!=shared_sources[handle-0x4000])) view++;
 if(view==64) boundary("shared-unmap-missing",&state,state.pc);
 if(shared_views[view].size!=size) boundary("shared-unmap-size",&state,state.pc);
 for(uint64_t offset=0;offset<size;offset+=4096) {
 uint64_t page_address=address+offset,base=page_address&~65535ull;unsigned granule=0;
 while(granule<64 && shared_granules[granule].base!=base) granule++;
 if(granule==64 || !shared_unmap2(GetCurrentProcess(),(void*)(uintptr_t)page_address,MEM_PRESERVE_PLACEHOLDER)) boundary("host-shared-unmap",&state,state.pc);
 shared_granules[granule].occupied&=~(1u<<((page_address-base)/4096));
 if(!shared_granules[granule].occupied) {
  for(unsigned page=0;page<16;page++)
   if(!VirtualFree((void*)(uintptr_t)(base+page*4096),0,MEM_RELEASE)) boundary("host-shared-placeholder-release",&state,state.pc);
  shared_granules[granule].base=0;
 }
 }
 memset(&shared_views[view],0,sizeof(shared_views[view]));
}
