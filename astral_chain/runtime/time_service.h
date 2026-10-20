/* Host-backed clock memory. Layout follows libnx's lock-free time readers.
 * All guest execution and updates currently run on the one runtime thread. */
static HANDLE clock_memory;
static uint8_t *clock_page;
static uint8_t clock_source[16];
static int64_t clock_last_steady=-1,clock_last_offset;
static int64_t clock_steady_base;

static int64_t clock_unix_seconds(void) {
 FILETIME now;GetSystemTimePreciseAsFileTime(&now);
 uint64_t ticks=((uint64_t)now.dwHighDateTime<<32)|now.dwLowDateTime;
 return (int64_t)(ticks/10000000ull)-11644473600ll;
}
static void clock_sample(int64_t *wall,int64_t *counter) {
 FILETIME now;GetSystemTimePreciseAsFileTime(&now);
 uint64_t ticks=((uint64_t)now.dwHighDateTime<<32)|now.dwLowDateTime;
 __int128 ns=((__int128)ticks-116444736000000000ll)*100;
 if(ns<INT64_MIN || ns>INT64_MAX) boundary("host-clock-range",&state,state.pc);
 *wall=(int64_t)ns;*counter=(int64_t)(((__uint128_t)rt_ticks()*1000000000ull)/19200000ull);
}
static void clock_values(int64_t wall,int64_t counter,int64_t base,int64_t *steady,int64_t *offset) {
 __int128 ns=(__int128)counter+base,delta=(__int128)wall-ns;
 *steady=(int64_t)(ns/1000000000ll);
 /* Align at initialization, then round whole-second UTC adjustments rather
  * than repeatedly subtracting independently truncated clock readings. */
 *offset=(int64_t)((delta+(delta>=0 ? 500000000ll : -500000000ll))/1000000000ll);
}
static void clock_publish(size_t offset,const void *value,size_t size) {
 volatile LONG *counter=(volatile LONG*)(clock_page+offset);
 uint32_t next=(uint32_t)*counter+1;
 memcpy(clock_page+offset+8+(next&1)*size,value,size);
 InterlockedExchange(counter,(LONG)next);
}
static void clock_refresh(void) {
 if(!clock_page) return;
 int64_t wall,counter,steady,offset;clock_sample(&wall,&counter);
 clock_values(wall,counter,clock_steady_base,&steady,&offset);
 if(steady==clock_last_steady && offset==clock_last_offset) return;
 uint8_t context[32];memcpy(context,&offset,8);memcpy(context+8,&steady,8);memcpy(context+16,clock_source,16);
 clock_publish(0x38,context,sizeof(context));
 clock_publish(0x80,context,sizeof(context));
 clock_last_steady=steady;clock_last_offset=offset;
}
static void clock_initialize(void) {
 if(clock_page) return;
 clock_memory=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,4096,NULL);
 if(!clock_memory) boundary("host-clock-memory-create",&state,state.pc);
 clock_page=MapViewOfFile(clock_memory,FILE_MAP_WRITE,0,0,4096);
 if(!clock_page) boundary("host-clock-memory-view",&state,state.pc);
 if(BCryptGenRandom(NULL,clock_source,sizeof(clock_source),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)
  boundary("host-clock-source-id",&state,state.pc);
 int64_t wall,counter;clock_sample(&wall,&counter);
 clock_steady_base=wall%1000000000ll-counter%1000000000ll;
 uint8_t steady[24]={0};memcpy(steady,&clock_steady_base,8);memcpy(steady+8,clock_source,16);
 clock_publish(0,steady,sizeof(steady));
 clock_last_steady=-1;
 clock_refresh();
 /* No console network-clock synchronization is claimed. Both clock contexts
  * use Windows UTC; automatic correction is disabled in this local profile. */
}
