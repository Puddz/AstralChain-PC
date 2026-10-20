/* One named display backed by the native application window. Opening it is
 * separate from creating layers, queuing buffers and presenting frames. */
static unsigned vi_display_refs;
static uint64_t vi_display_layer;
#include "buffer_queue.h"
#include "display_surface.h"
static struct { uint64_t id,aruid; unsigned binder,open_count,weak_refs,strong_refs; HANDLE buffer_event; uint16_t opens[64][33]; BqProducer producer; } vi_layers[16];
static uint64_t vi_next_layer_id=1;
static unsigned vi_next_binder_id=1;
static void vi_buffer_availability(unsigned i) {
 if(!vi_layers[i].buffer_event) return;
 BOOL ok=bq_has_free(&vi_layers[i].producer) ? SetEvent(vi_layers[i].buffer_event) : ResetEvent(vi_layers[i].buffer_event);
 if(!ok) boundary("vi-buffer-event-update",&state,state.pc);
}
static const char *vi_retain_buffer(void *context,BqGraphicBuffer *buffer) {
 (void)context;unsigned i=nvmap_find(buffer->object);
 if(i==256 || !nvmap_objects[i].allocated) return "graphic buffer nvmap object unavailable";
 if(buffer->offset>=nvmap_objects[i].size || buffer->size>nvmap_objects[i].size-buffer->offset)
  return "graphic buffer exceeds nvmap backing";
 uint64_t address=nvmap_objects[i].address+buffer->offset;Region backing=query(address);
 if(!(backing.permission&1) || address<backing.base || address-backing.base>=backing.size || buffer->size>backing.size-(address-backing.base))
  return "graphic buffer backing not readable";
 if(nvmap_objects[i].display_refs==UINT32_MAX) return "graphic buffer reference capacity";
 nvmap_objects[i].display_refs++;buffer->reference=i+1;return NULL;
}
static void vi_release_buffer(void *context,unsigned reference) {
 (void)context;
 if(!reference || reference>256 || !nvmap_objects[reference-1].display_refs) boundary("graphic-buffer-reference-invariant",&state,state.pc);
 nvmap_objects[reference-1].display_refs--;nvmap_collect(reference-1);
}
static const char *vi_consume_buffer(void *context,const BqGraphicBuffer *buffer,const BqFrameInput *frame) {
 uint64_t layer=(uint64_t)(uintptr_t)context;
 if(vi_display_layer && vi_display_layer!=layer) return "display layer composition unsupported";
 if(!frame->automatic || frame->transform || (frame->sticky!=0 && frame->sticky!=8) || frame->scaling!=1)
  return "unsupported display timestamp, transform or scaling mode";
 for(unsigned i=0;i<frame->fence_count;i++) {
  unsigned id=frame->fences[i].id,value=frame->fences[i].value;
  if(id==0xffffffffu) continue;
  if(!id || id>=192 || !gpu_syncpoints[id].owner) return "display fence syncpoint unavailable";
  if(value>gpu_syncpoints[id].completed) return "display fence not completed";
 }
 unsigned object=buffer->reference;
 if(!object || object>256) return "display buffer reference";
 object--;
 if(!nvmap_objects[object].allocated || !nvmap_objects[object].display_refs || nvmap_objects[object].id!=buffer->object ||
    nvmap_objects[object].storage!=GPU_STORAGE_CANONICAL) return "display backing unavailable or compressed";
 if(buffer->offset>nvmap_objects[object].size || buffer->size>nvmap_objects[object].size-buffer->offset) return "display backing extent";
 uint64_t address=nvmap_objects[object].address+buffer->offset;Region r=query(address);
 if(!(r.permission&1) || address<r.base || address-r.base>=r.size || buffer->size>r.size-(address-r.base)) return "display backing not readable";
 if(!app_window) return "display window unavailable";
 if(!app_stretch_dib) {
  HMODULE gdi=LoadLibraryW(L"gdi32.dll");
  if(!gdi) return "display GDI unavailable";
  app_stretch_dib=(void*)GetProcAddress(gdi,"StretchDIBits");
  if(!app_stretch_dib) {FreeLibrary(gdi);return "display blit unavailable";}
 }
 unsigned width=frame->right-frame->left,height=frame->bottom-frame->top;
 uint64_t bytes=(uint64_t)width*height*4;
 if(!width || !height || bytes>64*1024*1024 || app_frame_count==UINT64_MAX) return "display frame extent or counter";
 uint8_t *pixels=malloc((size_t)bytes);if(!pixels) return "display frame allocation";
 atomic_thread_fence(memory_order_seq_cst);
 const char *error=display_surface_decode(buffer,(void*)(uintptr_t)address,(size_t)buffer->size,
  frame->left,frame->top,frame->right,frame->bottom,pixels,(size_t)bytes);
 if(error) {free(pixels);return error;}
 /* A 60-Hz compatibility display preserves the game's interval (2 -> 30 Hz).
  * This is host pacing, not measured gameplay FPS or physical-vblank locking. */
 uint64_t deadline=app_frame_tick+(uint64_t)frame->interval*320000;
 while(app_frame_tick && rt_ticks()<deadline) {app_pump();Sleep(1);}
 free(app_frame_pixels);app_frame_pixels=pixels;app_frame_width=width;app_frame_height=height;
 app_frame_tick=rt_ticks();app_frame_count++;
 vi_display_layer=layer;
 SetWindowTextW(app_window,L"Astral Chain native port - display buffer");
 InvalidateRect(app_window,NULL,FALSE);UpdateWindow(app_window);
 printf("VI consumed frame=%llu size=%ux%u interval=%u fences=%u\n",(unsigned long long)app_frame_count,width,height,frame->interval,frame->fence_count);
 return NULL;
}
static unsigned vi_binder_index(unsigned binder) {
 for(unsigned i=0;i<16;i++) if(vi_layers[i].id && vi_layers[i].binder==binder) return i;
 boundary("vi-binder-id",&state,state.pc);
}
static unsigned vi_layer_index(uint64_t id) {
 for(unsigned i=0;i<16;i++) if(vi_layers[i].id==id && id) return i;
 boundary("vi-layer-id",&state,state.pc);
}
static uint64_t vi_create_managed_layer(void) {
 unsigned i=0;while(i<16 && vi_layers[i].id) i++;
 if(i==16 || vi_next_binder_id>=0x7fffffff) boundary("vi-layer-limit",&state,state.pc);
 app_open();memset(&vi_layers[i],0,sizeof(vi_layers[i]));
 vi_layers[i].id=vi_next_layer_id++;vi_layers[i].binder=vi_next_binder_id++;
 vi_layers[i].aruid=GetCurrentProcessId();
 /* Initial BufferQueue default size; explicit producer buffers will provide
  * their own dimensions. This is not the native window's client size. */
 vi_layers[i].producer.width=1;vi_layers[i].producer.height=1;
 vi_layers[i].producer.format=1; /* HAL RGBA_8888 default, not a rendered buffer. */
 vi_layers[i].producer.retain=vi_retain_buffer;vi_layers[i].producer.release=vi_release_buffer;
 vi_layers[i].producer.consume=vi_consume_buffer;
 vi_layers[i].producer.context=(void*)(uintptr_t)vi_layers[i].id;
 printf("VI managed layer=%llu binder=%u\n",(unsigned long long)vi_layers[i].id,vi_layers[i].binder);
 return vi_layers[i].id;
}
static void vi_release_layers(unsigned session,unsigned object) {
 for(unsigned i=0;i<16;i++) {
  vi_layers[i].open_count-=vi_layers[i].opens[session][object];vi_layers[i].opens[session][object]=0;
 }
}
static void vi_convert_layers(unsigned session) {
 for(unsigned i=0;i<16;i++) {
  vi_layers[i].opens[session][1]=vi_layers[i].opens[session][0];vi_layers[i].opens[session][0]=0;
 }
}
static void vi_release_displays(unsigned *count) {
 if(*count>vi_display_refs) boundary("vi-display-reference-state",&state,state.pc);
 vi_display_refs-=*count;*count=0;
}
static void vi_reply_object(unsigned session,uint8_t *buffer,const IpcRequest *request,int kind,unsigned policy,unsigned level) {
 unsigned child=reply_object(session,buffer,request,kind);
 if(request->domain) {sessions[session].object_vi_policy[child-1]=policy;sessions[session].object_vi_level[child-1]=level;}
 else {unsigned i=session_index(child);sessions[i].vi_policy=policy;sessions[i].vi_level=level;}
}
static void vi_dispatch(unsigned session,int kind,uint8_t *buffer,const IpcRequest *request) {
 int opening=kind==SESSION_VI_APPLICATION && request->command==2020;
 int transaction=kind==SESSION_VI_RELAY && (request->command==0 || request->command==3);
 if(request->copy_count || (!transaction && request->send_size) || request->input_object_count ||
    (!opening && request->send_pid) || (!opening && !transaction && request->receive_count))
  boundary("vi-request-layout",&state,state.pc);
 if(kind==SESSION_VI_RELAY && request->command==1) {
  if(request->argument_size<12) boundary("vi-binder-reference-layout",&state,state.pc);
  unsigned i=vi_binder_index(ipc_u32(request->arguments)),type=ipc_u32(request->arguments+8);
  int32_t delta=(int32_t)ipc_u32(request->arguments+4);
  if(type>1) boundary("vi-binder-reference-type",&state,state.pc);
  unsigned *refs=type ? &vi_layers[i].strong_refs : &vi_layers[i].weak_refs;
  int64_t result=(int64_t)*refs+delta;
  if(result<0 || result>1048576) boundary("vi-binder-reference-count",&state,state.pc);
  *refs=(unsigned)result;reply_result(buffer,request,0);return;
 }
 if(kind==SESSION_VI_RELAY && request->command==2) {
  if(request->argument_size<8 || ipc_u32(request->arguments+4)!=0xf) boundary("vi-binder-event-selector",&state,state.pc);
  unsigned i=vi_binder_index(ipc_u32(request->arguments));
  if(!vi_layers[i].strong_refs) boundary("vi-binder-unreferenced",&state,state.pc);
  unsigned handle=copy_event(&vi_layers[i].buffer_event);
  vi_buffer_availability(i);
  ipc_reply_copy(buffer,256,handle,request->domain,request->token);return;
 }
 if(transaction) {
  if(request->argument_size<12 || request->send_size<16 || request->send_size>4096 || request->receive_count!=1 || request->receive_size<16)
   boundary("vi-binder-transaction-layout",&state,state.pc);
  unsigned i=vi_binder_index(ipc_u32(request->arguments));
  if(!vi_layers[i].strong_refs) boundary("vi-binder-unreferenced",&state,state.pc);
  Region input=query(request->send_address),output=query(request->receive_address);
  if(!(input.permission&1) || request->send_size>input.size-(request->send_address-input.base) ||
     !(output.permission&2) || request->receive_size>output.size-(request->receive_address-output.base))
   boundary("vi-binder-transaction-memory",&state,state.pc);
  printf("BINDER transaction id=%u code=%u flags=0x%x input=%llu output=%llu\n",vi_layers[i].binder,
   ipc_u32(request->arguments+4),ipc_u32(request->arguments+8),(unsigned long long)request->send_size,(unsigned long long)request->receive_size);
  unsigned written=0;
  const char *error=bq_transaction(&vi_layers[i].producer,ipc_u32(request->arguments+4),ipc_u32(request->arguments+8),
   (void*)(uintptr_t)request->send_address,(size_t)request->send_size,
   (void*)(uintptr_t)request->receive_address,(size_t)request->receive_size,&written);
  if(error) {
   printf("BINDER boundary: %s\n",error);
   unsigned captured=request->send_size<256 ? (unsigned)request->send_size : 256;
   printf("BINDER input captured=%u total=%llu hex=",captured,(unsigned long long)request->send_size);
   for(unsigned j=0;j<captured;j++) printf("%02x",((uint8_t*)(uintptr_t)request->send_address)[j]);
   puts("");boundary("vi-binder-transaction",&state,state.pc);
  }
  vi_buffer_availability(i);
  printf("BINDER producer api=%u controlled=%u reply_bytes=%u\n",vi_layers[i].producer.api,vi_layers[i].producer.controlled,written);
  reply_result(buffer,request,0);return;
 }
 unsigned policy=request->domain ? sessions[session].object_vi_policy[request->object_id-1] : sessions[session].vi_policy;
 unsigned level=request->domain ? sessions[session].object_vi_level[request->object_id-1] : sessions[session].vi_level;
 unsigned *displays=request->domain ? &sessions[session].object_vi_displays[request->object_id-1] : &sessions[session].vi_displays;
 if(opening) {
  if(!*displays || request->argument_size<80 || memcmp(request->arguments,"Default\0",8) || !request->send_pid || request->receive_count!=1)
   boundary("vi-open-layer-layout",&state,state.pc);
  uint64_t id,aruid;memcpy(&id,request->arguments+64,8);memcpy(&aruid,request->arguments+72,8);
  unsigned i=vi_layer_index(id),object=request->domain ? request->object_id : 0;
  if(aruid!=vi_layers[i].aruid) boundary("vi-layer-owner",&state,state.pc);
  Region destination=query(request->receive_address);
  if(request->receive_size<60 || !(destination.permission&2) || 60>destination.size-(request->receive_address-destination.base))
   boundary("vi-native-window-output",&state,state.pc);
  if(vi_layers[i].opens[session][object]>=32) boundary("vi-layer-open-limit",&state,state.pc);
  uint8_t parcel[60]={0};
  ipc_put32(parcel,40);ipc_put32(parcel+4,16);ipc_put32(parcel+8,4);ipc_put32(parcel+12,56);
  ipc_put32(parcel+16,2);ipc_put32(parcel+20,GetCurrentProcessId());ipc_put32(parcel+24,vi_layers[i].binder);
  memcpy(parcel+40,"dispdrv",7);
  memcpy((void*)(uintptr_t)request->receive_address,parcel,sizeof(parcel));
  vi_layers[i].opens[session][object]++;vi_layers[i].open_count++;
  reply_wide_value(buffer,request,sizeof(parcel));return;
 }
 if((kind==SESSION_VI_APPLICATION && request->command==2021) || (kind==SESSION_VI_MANAGER && request->command==2011)) {
  if(request->argument_size<8) boundary("vi-layer-close-layout",&state,state.pc);
  uint64_t id;memcpy(&id,request->arguments,8);unsigned i=vi_layer_index(id),object=request->domain ? request->object_id : 0;
  if(kind==SESSION_VI_MANAGER) {
   if(vi_layers[i].open_count) boundary("vi-layer-destroy-open",&state,state.pc);
   if(vi_layers[i].weak_refs || vi_layers[i].strong_refs) boundary("vi-layer-destroy-binder",&state,state.pc);
   if(vi_layers[i].producer.api) boundary("vi-layer-destroy-connected",&state,state.pc);
   if(vi_display_layer==id) {
    free(app_frame_pixels);app_frame_pixels=NULL;app_frame_width=app_frame_height=0;app_frame_tick=0;vi_display_layer=0;
    InvalidateRect(app_window,NULL,TRUE);
   }
   if(vi_layers[i].buffer_event && !CloseHandle(vi_layers[i].buffer_event)) boundary("vi-layer-event-close",&state,state.pc);
   memset(&vi_layers[i],0,sizeof(vi_layers[i]));
  } else {
   if(!vi_layers[i].opens[session][object]) boundary("vi-layer-not-open",&state,state.pc);
   vi_layers[i].opens[session][object]--;vi_layers[i].open_count--;
  }
  reply_result(buffer,request,0);return;
 }
 if(kind==SESSION_VI_APPLICATION && request->command==2102) {
  if(request->argument_size<4) boundary("vi-scaling-layout",&state,state.pc);
  unsigned mode=ipc_u32(request->arguments);
  /* VI accepts FitToLayer (2) and PreserveAspectRatio (4). The producer
   * enum uses 1 and 4 respectively; the result is a 64-bit wire value. */
  if(mode>4) {reply_result(buffer,request,0x272);return;}
  if(mode!=2 && mode!=4) {reply_result(buffer,request,0xc72);return;}
  printf("VI scaling mode=%u producer=%u\n",mode,mode==2 ? 1 : 4);
  reply_wide_value(buffer,request,mode==2 ? 1 : 4);return;
 }
 if(kind==SESSION_VI_APPLICATION && request->command==1010) {
  if(request->argument_size<64 || memcmp(request->arguments,"Default\0",8)) boundary("vi-display-name",&state,state.pc);
  if(*displays>=32 || vi_display_refs>=1024) boundary("vi-display-reference-limit",&state,state.pc);
  app_open();if(!app_window) boundary("vi-display-window",&state,state.pc);
  (*displays)++;vi_display_refs++;
  reply_wide_value(buffer,request,1);puts("VI opened Default display=1");return;
 }
 if(kind==SESSION_VI_APPLICATION && request->command==1020) {
  uint64_t id=0;if(request->argument_size>=8) memcpy(&id,request->arguments,8);
  if(id!=1 || !*displays) boundary("vi-close-display",&state,state.pc);
  (*displays)--;vi_display_refs--;reply_result(buffer,request,0);return;
 }
 if(kind==SESSION_VI_ROOT && request->command==level) {
  if(request->argument_size<4 || ipc_u32(request->arguments)>1) boundary("vi-service-policy",&state,state.pc);
  if(!level && ipc_u32(request->arguments)) {reply_result(buffer,request,0xa72);return;}
  vi_reply_object(session,buffer,request,SESSION_VI_APPLICATION,ipc_u32(request->arguments),level);return;
 }
 if(kind==SESSION_VI_APPLICATION && request->command>=100 && request->command<=103) {
  if((request->command!=100 && !level) || (request->command==102 && level<2)) {reply_result(buffer,request,0xa72);return;}
  const int children[]={SESSION_VI_RELAY,SESSION_VI_SYSTEM,SESSION_VI_MANAGER,SESSION_VI_INDIRECT};
  vi_reply_object(session,buffer,request,children[request->command-100],policy,level);return;
 }
 boundary("vi-command",&state,state.pc);
}
