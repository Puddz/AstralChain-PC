/* Software channel queue. Submission and execution are separate operations. */
#include "gpu_engine.h"
typedef struct {
 GpuEngineState engine;
 uint64_t *entries;
 unsigned capacity,flags,syncpoint,read_index,write_index,pending;
 uint64_t top_level_get;
 unsigned timeout,timeout_set,priority,priority_set;
 unsigned timeslice_us,timeslice_set;
 unsigned notifier_object;
 uint64_t notifier_address,notifier_size;
 uint64_t user_data;
 GpuAddressSpace *zcull_space;
 unsigned zcull_mapping,zcull_mode;
 uint64_t zcull_address,zcull_available;
 struct { unsigned class_id,flags;uint64_t id; } contexts[6];
} GpuChannel;
static uint64_t gpu_next_context=1;
/* IDs are not recycled while stale guest fences might still refer to them.
 * Exhaustion is explicit until fence/event retirement is implemented. */
static struct { unsigned owner,completed,submitted; } gpu_syncpoints[192];
static unsigned gpu_next_syncpoint=1;
static const char *gpu_check_submission(GpuChannel *channel,unsigned count,unsigned flags,unsigned increments) {
 if(!channel->entries || !channel->syncpoint || channel->syncpoint>=192 || !gpu_syncpoints[channel->syncpoint].owner)
  return "submission channel unavailable";
 if(flags!=4 && flags!=0x104) return "unsupported submission flags";
 if(!count || count>channel->capacity) return "submission queue extent";
 if(channel->pending || channel->engine.pending_work || channel->engine.inline_dma.active || channel->engine.macro_active)
  return "submission channel busy";
 unsigned id=channel->syncpoint;
 if(gpu_syncpoints[id].completed!=gpu_syncpoints[id].submitted) return "submission fence outstanding";
 if(flags==0x104 && increments>UINT32_MAX-gpu_syncpoints[id].submitted) return "submission fence wrap unsupported";
 return NULL;
}
/* Caller preflights every descriptor before publishing the reserved target. */
static void gpu_begin_submission(GpuChannel *channel,const uint64_t *entries,unsigned count,unsigned flags,unsigned increments) {
 for(unsigned j=0;j<count;j++) {
  channel->entries[channel->write_index]=entries[j];
  channel->write_index=(channel->write_index+1)%channel->capacity;
 }
 channel->pending=count;
 if(flags==0x104) gpu_syncpoints[channel->syncpoint].submitted+=increments;
}
static void gpu_consume_descriptor(GpuChannel *channel) {
 channel->read_index=(channel->read_index+1)%channel->capacity;channel->pending--;
}
static const char *gpu_increment_channel_syncpoint(void *opaque,unsigned id) {
 GpuChannel *channel=opaque;
 if(!channel || !channel->entries || !id || id>=192 || channel->syncpoint!=id || !gpu_syncpoints[id].owner)
  return "syncpoint not owned by channel";
 if(channel->engine.pending_work || channel->engine.inline_dma.active) return "syncpoint work outstanding";
 /* Wrap and event waiters need explicit handling before they are admitted.
  * This records an executed increment, not submission or queue completion. */
 if(gpu_syncpoints[id].completed==UINT32_MAX) return "syncpoint wrap unsupported";
 atomic_thread_fence(memory_order_seq_cst);
 gpu_syncpoints[id].completed++;return NULL;
}
static unsigned gpu_allocate_fifo(GpuChannel *channel,unsigned owner,const uint32_t words[8]) {
 if(channel->entries) return 8;
 if(words[1]&~1u || words[2] || words[5] || words[6] || words[7]) boundary("nv-channel-fifo-options",&state,state.pc);
 unsigned count=words[0];
 if(count<2 || count>65536 || (count&(count-1))) return 4;
 if(gpu_next_syncpoint>=192) return 6;
 uint64_t *entries=calloc(count,sizeof(uint64_t));if(!entries) return 6;
 unsigned id=gpu_next_syncpoint++;
 gpu_syncpoints[id].owner=owner;
 channel->entries=entries;channel->capacity=count;channel->flags=words[1];channel->syncpoint=id;
 channel->engine.increment_syncpoint=gpu_increment_channel_syncpoint;channel->engine.syncpoint_context=channel;return 0;
}
static void gpu_close_channel(GpuChannel *channel) {
 if(channel->pending || channel->engine.pending_work || channel->engine.inline_dma.active)
  boundary("nv-channel-close-pending",&state,state.pc);
 if(channel->entries) gpu_syncpoints[channel->syncpoint].owner=0;
 if(channel->zcull_space) channel->zcull_space->mappings[channel->zcull_mapping].bound_refs--;
 if(channel->notifier_object) {unsigned object=channel->notifier_object-1;nvmap_objects[object].mapped_refs--;nvmap_collect(object);}
 free(channel->entries);memset(channel,0,sizeof(*channel));
}
static void nv_bind_zcull(GpuChannel *channel,GpuAddressSpace *space,uint8_t *buffer,const IpcRequest *request) {
 if(request->send_size!=16 || request->receive_count!=1 || request->receive_size<16) boundary("nv-zcull-bind-layout",&state,state.pc);
 Region source=query(request->send_address),target=query(request->receive_address);
 if(!(source.permission&1) || 16>source.size-(request->send_address-source.base) ||
    !(target.permission&2) || 16>target.size-(request->receive_address-target.base)) boundary("nv-zcull-bind-memory",&state,state.pc);
 uint32_t words[4];uint64_t address;memcpy(words,(void*)(uintptr_t)request->send_address,16);memcpy(&address,words,8);
 printf("GPU ZCULL address=0x%llx mode=%u reserved=%u\n",(unsigned long long)address,words[2],words[3]);
 if(words[2]!=2 || words[3]) boundary("nv-zcull-bind-options",&state,state.pc);
 if(!space || !channel->entries || !channel->contexts[1].id) {ipc_reply_u32(buffer,256,8,request->token);return;}
 if(!address || address%256) {ipc_reply_u32(buffer,256,4,request->token);return;}
 unsigned slot=GPU_AS_SLOTS;uint64_t available=0;
 for(unsigned i=0;i<GPU_AS_SLOTS;i++) if(space->mappings[i].size && address>=space->mappings[i].address) {
  uint64_t relative=address-space->mappings[i].address;
  if(relative<space->mappings[i].size) {slot=i;available=space->mappings[i].size-relative;break;}
 }
 if(slot==GPU_AS_SLOTS || available<gpu_zcull_context_size) {ipc_reply_u32(buffer,256,4,request->token);return;}
 /* Retain the actual mapped extent, not an inferred hardware context layout.
  * Physical ZCULL save/restore is not performed by this software binding. */
 space->mappings[slot].bound_refs++;
 if(channel->zcull_space) channel->zcull_space->mappings[channel->zcull_mapping].bound_refs--;
 channel->zcull_space=space;channel->zcull_mapping=slot;channel->zcull_mode=words[2];
 channel->zcull_address=address;channel->zcull_available=available;
 memcpy((void*)(uintptr_t)request->receive_address,words,16);ipc_reply_u32(buffer,256,0,request->token);
}
static void nv_configure_channel(GpuChannel *channel,uint8_t *buffer,const IpcRequest *request,int priority) {
 if(request->send_size!=4 || request->receive_size) boundary("nv-channel-config-layout",&state,state.pc);
 Region source=query(request->send_address);
 if(!(source.permission&1) || 4>source.size-(request->send_address-source.base)) boundary("nv-channel-config-memory",&state,state.pc);
 unsigned value=ipc_u32((void*)(uintptr_t)request->send_address);
 printf("GPU channel %s=%u\n",priority ? "priority" : "timeout",value);
 if(priority) {
  if(value!=50 && value!=100 && value!=150) {ipc_reply_u32(buffer,256,4,request->token);return;}
  channel->priority=value;channel->priority_set=1;
  channel->timeslice_us=value==50 ? 1300 : value==100 ? 2600 : 5200;channel->timeslice_set=1;
 } else {channel->timeout=value;channel->timeout_set=1;}
 ipc_reply_u32(buffer,256,0,request->token);
}
static void nv_set_channel_timeslice(GpuChannel *channel,uint8_t *buffer,const IpcRequest *request) {
 if(request->send_size!=4 || request->receive_count!=1 || request->receive_size<4) boundary("nv-channel-timeslice-layout",&state,state.pc);
 Region source=query(request->send_address),target=query(request->receive_address);
 if(!(source.permission&1) || 4>source.size-(request->send_address-source.base) ||
    !(target.permission&2) || 4>target.size-(request->receive_address-target.base)) boundary("nv-channel-timeslice-memory",&state,state.pc);
 unsigned value=ipc_u32((void*)(uintptr_t)request->send_address);
 printf("GPU channel timeslice_us=%u\n",value);
 if(value<1000 || value>50000) {ipc_reply_u32(buffer,256,4,request->token);return;}
 channel->timeslice_us=value;channel->timeslice_set=1;
 memcpy((void*)(uintptr_t)request->receive_address,&value,4);ipc_reply_u32(buffer,256,0,request->token);
}
static void nv_set_channel_user_data(GpuChannel *channel,uint8_t *buffer,const IpcRequest *request) {
 if(request->send_size!=8 || request->receive_size) boundary("nv-channel-user-data-layout",&state,state.pc);
 Region source=query(request->send_address);
 if(!(source.permission&1) || 8>source.size-(request->send_address-source.base)) boundary("nv-channel-user-data-memory",&state,state.pc);
 /* Opaque guest association, never a host pointer to dereference. */
 memcpy(&channel->user_data,(void*)(uintptr_t)request->send_address,8);
 printf("GPU channel user_data=0x%llx\n",(unsigned long long)channel->user_data);
 ipc_reply_u32(buffer,256,0,request->token);
}
static void nv_configure_error_notifier(GpuChannel *channel,unsigned client,uint8_t *buffer,const IpcRequest *request) {
 if(request->send_size!=24 || request->receive_count!=1 || request->receive_size<24) boundary("nv-channel-notifier-layout",&state,state.pc);
 Region source=query(request->send_address),target=query(request->receive_address);
 if(!(source.permission&1) || 24>source.size-(request->send_address-source.base) ||
    !(target.permission&2) || 24>target.size-(request->receive_address-target.base)) boundary("nv-channel-notifier-memory",&state,state.pc);
 uint32_t words[6];memcpy(words,(void*)(uintptr_t)request->send_address,24);
 uint64_t offset,size;memcpy(&offset,words,8);memcpy(&size,words+2,8);
 printf("GPU error notifier object=%u offset=0x%llx size=0x%llx\n",words[4],(unsigned long long)offset,(unsigned long long)size);
 if(words[5]) boundary("nv-channel-notifier-options",&state,state.pc);
 unsigned object=256;
 if(words[4]) {
  object=nvmap_find(words[4]);
  if(!client || object==256 || !nvmap_objects[object].allocated || !nvmap_objects[object].refs[client-1] ||
     size<16 || offset>nvmap_objects[object].size || size>nvmap_objects[object].size-offset) {ipc_reply_u32(buffer,256,4,request->token);return;}
  nvmap_objects[object].mapped_refs++;
 }
 if(channel->notifier_object) {unsigned old=channel->notifier_object-1;nvmap_objects[old].mapped_refs--;nvmap_collect(old);}
 channel->notifier_object=object==256 ? 0 : object+1;
 channel->notifier_address=object==256 ? 0 : nvmap_objects[object].address+offset;
 channel->notifier_size=object==256 ? 0 : size;
 if(channel->notifier_object) memset((void*)(uintptr_t)channel->notifier_address,0,16);
 memcpy((void*)(uintptr_t)request->receive_address,words,24);ipc_reply_u32(buffer,256,0,request->token);
}
static void nv_allocate_fifo(GpuChannel *channel,unsigned owner,int bound,uint8_t *buffer,const IpcRequest *request) {
 if(request->send_size!=32 || request->receive_count!=1 || request->receive_size<32) boundary("nv-channel-fifo-layout",&state,state.pc);
 Region source=query(request->send_address),target=query(request->receive_address);
 if(!(source.permission&1) || 32>source.size-(request->send_address-source.base) ||
    !(target.permission&2) || 32>target.size-(request->receive_address-target.base)) boundary("nv-channel-fifo-memory",&state,state.pc);
 uint32_t words[8];memcpy(words,(void*)(uintptr_t)request->send_address,32);
 unsigned result=bound ? gpu_allocate_fifo(channel,owner,words) : 8;
 if(!result) {
  words[3]=channel->syncpoint;words[4]=gpu_syncpoints[channel->syncpoint].completed;
  memcpy((void*)(uintptr_t)request->receive_address,words,32);
 }
 ipc_reply_u32(buffer,256,result,request->token);
}
static void nv_allocate_context(GpuChannel *channel,uint8_t *buffer,const IpcRequest *request) {
 if(request->send_size!=16 || request->receive_count!=1 || request->receive_size<16) boundary("nv-channel-context-layout",&state,state.pc);
 Region source=query(request->send_address),target=query(request->receive_address);
 if(!(source.permission&1) || 16>source.size-(request->send_address-source.base) ||
    !(target.permission&2) || 16>target.size-(request->receive_address-target.base)) boundary("nv-channel-context-memory",&state,state.pc);
 uint32_t words[4];memcpy(words,(void*)(uintptr_t)request->send_address,16);
 printf("GPU context class=0x%x flags=0x%x\n",words[0],words[1]);
 if(!channel->entries) {ipc_reply_u32(buffer,256,8,request->token);return;}
 if(words[1]) boundary("nv-channel-context-flags",&state,state.pc);
 const unsigned classes[]={gpu_model.class_2d,gpu_model.class_3d,gpu_model.class_compute,gpu_model.class_gpfifo,gpu_model.class_inline,gpu_model.class_dma};
 unsigned slot=0;while(slot<6 && classes[slot]!=words[0]) slot++;
 if(slot==6) {ipc_reply_u32(buffer,256,4,request->token);return;}
 if(channel->contexts[slot].id) {ipc_reply_u32(buffer,256,8,request->token);return;}
 if(!gpu_next_context || gpu_next_context==UINT64_MAX) {ipc_reply_u32(buffer,256,6,request->token);return;}
 uint64_t id=gpu_next_context++;
 channel->contexts[slot].class_id=words[0];channel->contexts[slot].flags=words[1];channel->contexts[slot].id=id;
 memcpy(words+2,&id,8);memcpy((void*)(uintptr_t)request->receive_address,words,16);
 ipc_reply_u32(buffer,256,0,request->token);
}
