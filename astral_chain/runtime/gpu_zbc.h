/* Retained clear-value registrations for the software GPU. Color and depth
 * have independent 15-entry capacities. These are not host GPU commands. */
typedef struct { uint32_t ds[4],l2[4],depth,format,refs; } GpuClearValue;
static GpuClearValue gpu_clear_values[2][15];
static unsigned gpu_register_clear(const uint32_t data[11]) {
 unsigned type=data[10];if(type!=1 && type!=2) return 4;
 unsigned slot=15;
 for(unsigned i=0;i<15;i++) {
  GpuClearValue *value=&gpu_clear_values[type-1][i];
  if(!value->refs) {if(slot==15) slot=i;continue;}
  if(value->format!=data[9]) continue;
  if(type==1 ? memcmp(value->ds,data,16)!=0 : value->depth!=data[8]) continue;
  if(type==1 && memcmp(value->l2,data+4,16)) return 4;
  if(value->refs==0xffffffffu) return 6;
  value->refs++;return 0;
 }
 if(slot==15) return 6;
 GpuClearValue value={0};value.format=data[9];value.refs=1;
 if(type==1) {memcpy(value.ds,data,16);memcpy(value.l2,data+4,16);} else value.depth=data[8];
 gpu_clear_values[type-1][slot]=value;return 0;
}
static void nv_set_zbc(uint8_t *buffer,const IpcRequest *request) {
 if(request->send_size!=44 || request->receive_size) boundary("nv-zbc-set-layout",&state,state.pc);
 Region source=query(request->send_address);
 if(!(source.permission&1) || 44>source.size-(request->send_address-source.base)) boundary("nv-zbc-set-memory",&state,state.pc);
 uint32_t data[11];memcpy(data,(void*)(uintptr_t)request->send_address,44);
 printf("ZBC register type=%u format=0x%x depth=0x%x ds=%08x,%08x,%08x,%08x l2=%08x,%08x,%08x,%08x\n",
  data[10],data[9],data[8],data[0],data[1],data[2],data[3],data[4],data[5],data[6],data[7]);
 ipc_reply_u32(buffer,256,gpu_register_clear(data),request->token);
}
