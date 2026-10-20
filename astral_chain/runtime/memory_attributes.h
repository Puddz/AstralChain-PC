/* Guest cache-policy metadata. Host CPU backing is coherent; GPU uploads and
 * downloads must synchronize explicitly when command execution is added. */
typedef struct { uint64_t begin,end; } UncachedRange;
static UncachedRange uncached_ranges[512];
static unsigned uncached_count;
static unsigned memory_attribute(uint64_t address) {
 for(unsigned i=0;i<uncached_count;i++) if(address>=uncached_ranges[i].begin && address<uncached_ranges[i].end) return 8;
 return 0;
}
static int uncached_overlap(uint64_t address,uint64_t size) {
 for(unsigned i=0;i<uncached_count;i++) if(address<uncached_ranges[i].end && uncached_ranges[i].begin<address+size) return 1;
 return 0;
}
static Region attribute_clip(Region region,uint64_t address) {
 uint64_t begin=region.base,end=begin+region.size;
 for(unsigned i=0;i<uncached_count;i++) {
  uint64_t a=uncached_ranges[i].begin,b=uncached_ranges[i].end;
  if(address<a && a<end) end=a;
  if(address>=b && b>begin) begin=b;
  if(address>=a && address<b) {if(a>begin) begin=a;if(b<end) end=b;}
 }
 region.base=begin;region.size=end-begin;return region;
}
static unsigned update_uncached(uint64_t begin,uint64_t end,int enabled) {
 UncachedRange next[514];unsigned count=0;
 for(unsigned i=0;i<uncached_count;i++) {
  UncachedRange r=uncached_ranges[i];
  if(r.end<=begin || r.begin>=end) next[count++]=r;
  else {
   if(r.begin<begin) next[count++]=(UncachedRange){r.begin,begin};
   if(r.end>end) next[count++]=(UncachedRange){end,r.end};
  }
 }
 if(enabled) next[count++]=(UncachedRange){begin,end};
 for(unsigned i=1;i<count;i++) {
  UncachedRange r=next[i];unsigned j=i;while(j && next[j-1].begin>r.begin) {next[j]=next[j-1];j--;}next[j]=r;
 }
 unsigned merged=0;
 for(unsigned i=0;i<count;i++) {
  if(merged && next[merged-1].end>=next[i].begin) {if(next[i].end>next[merged-1].end) next[merged-1].end=next[i].end;}
  else next[merged++]=next[i];
 }
 if(merged>512) return 0xce01;
 memcpy(uncached_ranges,next,merged*sizeof(*next));uncached_count=merged;return 0;
}
