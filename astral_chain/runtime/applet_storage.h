/* Host-owned applet storage. Service objects/accessors hold counted references;
 * payloads are never persisted to source files. */
static struct { uint8_t *data; uint64_t size; unsigned refs; } applet_storage[16];
static unsigned storage_create(uint64_t size) {
 if(size>1024*1024) boundary("applet-storage-size",&state,state.pc);
 for(unsigned i=0;i<16;i++) if(!applet_storage[i].data) {
  applet_storage[i].data=calloc(1,size ? (size_t)size : 1);
  if(!applet_storage[i].data) boundary("host-applet-storage-allocation",&state,state.pc);
  applet_storage[i].size=size;applet_storage[i].refs=0;return i+1;
 }
 boundary("applet-storage-limit",&state,state.pc);
}
static void storage_validate(unsigned id) {
 if(!id || id>16 || !applet_storage[id-1].data) boundary("invalid-applet-storage",&state,state.pc);
}
static void storage_retain(unsigned id) {storage_validate(id);applet_storage[id-1].refs++;}
static void storage_release(unsigned id) {
 storage_validate(id);
 if(!applet_storage[id-1].refs) boundary("applet-storage-reference-underflow",&state,state.pc);
 if(!--applet_storage[id-1].refs) {free(applet_storage[id-1].data);memset(&applet_storage[id-1],0,sizeof(applet_storage[id-1]));}
}
