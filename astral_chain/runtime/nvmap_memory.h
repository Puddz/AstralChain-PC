/* Retained guest heap backing remains CPU-accessible. Pins forbid decommit or
 * ownership transfer, rather than copying or allocating replacement bytes. */
/* Native producers retain canonical bytes, not NVIDIA compression metadata.
 * Any future compressed producer must change this tag before writing. */
enum { GPU_STORAGE_UNKNOWN, GPU_STORAGE_CANONICAL };
static struct {
 unsigned id,size,allocated,alignment,flags,kind,mapped_refs,display_refs,storage;
 uint64_t address;
 uint16_t refs[64];
} nvmap_objects[256];
static int nvmap_overlaps(uint64_t address,uint64_t size) {
 if(!size) return 0;
 for(unsigned i=0;i<256;i++) if(nvmap_objects[i].allocated) {
  uint64_t a=nvmap_objects[i].address,n=nvmap_objects[i].size;
  if(address<=a ? a-address<size : address-a<n) return 1;
 }
 return 0;
}
static int nvmap_above(uint64_t end) {
 for(unsigned i=0;i<256;i++) if(nvmap_objects[i].allocated && nvmap_objects[i].address+nvmap_objects[i].size>end) return 1;
 return 0;
}
