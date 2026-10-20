/* Captured legacy OpenAudioOut contract; additional stream commands remain
 * explicit boundaries. Slot addresses stay stable for the Windows callback. */
#include "wave_output.h"
/* dropped: tags of buffers lost when an invalidated host device was reopened; reported released next. */
static struct { unsigned active; WaveOutput wave; uint64_t dropped[32]; unsigned ndropped; } audio_outputs[16];
static void audout_release(unsigned *id) {
 if(!*id)return;
 if(*id>16 || !audio_outputs[*id-1].active) boundary("audout-owner",&state,state.pc);
 MMRESULT result=wave_output_close(&audio_outputs[*id-1].wave);
 if(result) {printf("AUDOUT close result=%u\n",result);boundary("audout-host-close",&state,state.pc);}
 audio_outputs[*id-1].active=0;*id=0;
}
static void audout_memory(uint64_t address,uint64_t size,unsigned permission,const char *reason) {
 Region region=query(address);
 if(!size || !(region.permission&permission) || address<region.base ||
    address-region.base>=region.size || size>region.size-(address-region.base))
  boundary(reason,&state,state.pc);
}
static void audout_stream_dispatch(unsigned session,uint8_t *buffer,const IpcRequest *request) {
 audren_capture("output stream",request);
 unsigned id=request->domain ? sessions[session].object_audio_output[request->object_id-1] : sessions[session].audio_output;
 if(!id || id>16 || !audio_outputs[id-1].active) boundary("audout-owner",&state,state.pc);
 WaveOutput *wave=&audio_outputs[id-1].wave;
 if(request->command==3 || request->command==7) {
  if(request->argument_size<8 || request->argument_size>24 || request->send_pid ||
     request->send_count!=1 || request->send_size!=40 || request->receive_count ||
     request->copy_count || request->input_object_count || request->receive_mode ||
     (request->command==3 && request->send_is_pointer))
   boundary("audout-append-layout",&state,state.pc);
  audout_memory(request->send_address,40,1,"audout-buffer-not-readable");
  uint64_t descriptor[5],tag;
  memcpy(descriptor,(const void*)(uintptr_t)request->send_address,40);
  memcpy(&tag,request->arguments,8);
  HOT_LOG("AUDOUT append tag=0x%llx next=0x%llx samples=0x%llx capacity=%llu size=%llu offset=%llu pointer=%u\n",
   (unsigned long long)tag,(unsigned long long)descriptor[0],(unsigned long long)descriptor[1],
   (unsigned long long)descriptor[2],(unsigned long long)descriptor[3],(unsigned long long)descriptor[4],request->send_is_pointer);
  /* Nonzero offset/next semantics have not been established for this SDK. */
  if(descriptor[0] || descriptor[4]) boundary("audout-buffer-offset-or-chain",&state,state.pc);
  if(!descriptor[3] || descriptor[3]>descriptor[2] || descriptor[3]>16u*1024*1024 ||
     descriptor[3]%wave->format.nBlockAlign)
   boundary("audout-buffer-size",&state,state.pc);
  audout_memory(descriptor[1],descriptor[3],1,"audout-samples-not-readable");
  for(unsigned i=0;i<wave->count;i++) if(wave->buffers[(wave->head+i)%32].tag==tag)
   boundary("audout-duplicate-buffer-tag",&state,state.pc);
  MMRESULT result=wave_output_append(wave,tag,(const void*)(uintptr_t)descriptor[1],(size_t)descriptor[3]);
  if(result==MMSYSERR_INVALHANDLE || result==MMSYSERR_NODRIVER) {
   /* The host device went away: reopen it with the same format and append again. */
   unsigned rate=wave->format.nSamplesPerSec,channels=wave->format.nChannels;int running=wave->running;
   unsigned *nd=&audio_outputs[id-1].ndropped;
   unsigned first=*nd;*nd+=wave_output_abandon(wave,audio_outputs[id-1].dropped+*nd,32-*nd);
   /* A failed append can leave its own buffer counted as queued: it is appended again below, so it is not
    * dropped (reporting it released while queued made the game reuse its tag: run 430). */
   for(unsigned i=first;i<*nd;) if(audio_outputs[id-1].dropped[i]==tag) audio_outputs[id-1].dropped[i]=audio_outputs[id-1].dropped[--*nd]; else i++;
   MMRESULT reopened=wave_output_open(wave,rate,channels);
   if(!reopened && running) reopened=wave_output_start(wave);
   printf("AUDOUT host device reopened after result=%u: %u\n",result,reopened);
   if(!reopened) result=wave_output_append(wave,tag,(const void*)(uintptr_t)descriptor[1],(size_t)descriptor[3]);
  }
  if(result) {printf("AUDOUT append result=%u\n",result);boundary("audout-host-append",&state,state.pc);}
  if(hot_log_on()) {
   WaveBuffer *owned=&wave->buffers[(wave->head+wave->count-1)%32];
   unsigned nonzero=0;for(unsigned i=0;i<owned->header.dwBufferLength;i++)
    nonzero+=((const unsigned char*)owned->header.lpData)[i]!=0;
   printf("AUDOUT queued frames=%u nonzero_bytes=%u outstanding=%u\n",
    owned->header.dwBufferLength/wave->format.nBlockAlign,nonzero,wave->count);
  }
  reply_result(buffer,request,0);return;
 }
 if(request->command==5 || request->command==8) {
  if(request->argument_size>8 || request->send_pid || request->send_count || request->receive_count!=1 ||
     request->copy_count || request->input_object_count || !request->receive_size ||
     request->receive_size%8 || request->receive_size>1048576 ||
     (request->command==5 && (request->receive_mode || request->receive_is_pointer)) ||
     (request->receive_is_pointer && request->receive_size>4096))
   boundary("audout-collect-layout",&state,state.pc);
  audout_memory(request->receive_address,request->receive_size,2,"audout-tags-not-writable");
  uint64_t tags[32];unsigned capacity=(unsigned)(request->receive_size/8),count=0;
  if(capacity>32)capacity=32;
  unsigned *nd=&audio_outputs[id-1].ndropped;
  while(*nd && count<capacity) tags[count++]=audio_outputs[id-1].dropped[--*nd];
  unsigned more=0;
  MMRESULT result=wave_output_collect(wave,tags+count,capacity-count,&more);count+=more;
  if(result) {printf("AUDOUT collect result=%u\n",result);boundary("audout-host-collect",&state,state.pc);}
  memset((void*)(uintptr_t)request->receive_address,0,(size_t)request->receive_size);
  memcpy((void*)(uintptr_t)request->receive_address,tags,count*8);
  HOT_LOG("AUDOUT released=%u outstanding=%u\n",count,wave->count);
  reply_value(buffer,request,count);return;
 }
 if(request->command==0 || request->command==1) {
  if(request->argument_size>8 || request->send_pid || request->send_count || request->receive_count ||
     request->copy_count || request->input_object_count || request->receive_mode)
   boundary("audout-state-layout",&state,state.pc);
  if(!request->command) {reply_value(buffer,request,wave->running?0:1);return;}
  MMRESULT result=wave_output_start(wave);
  if(result) {printf("AUDOUT start result=%u\n",result);boundary("audout-host-start",&state,state.pc);}
  puts("AUDOUT started");reply_result(buffer,request,0);return;
 }
 if(request->command!=4) boundary("audout-stream-command",&state,state.pc);
 if(request->argument_size>8 || request->send_pid || request->send_count || request->receive_count ||
    request->copy_count || request->input_object_count || request->receive_mode)
  boundary("audout-event-layout",&state,state.pc);
 /* The guest receives a duplicate; closing it cannot close the callback's event. */
 unsigned handle=copy_event(&audio_outputs[id-1].wave.completion);
 ipc_reply_copy(buffer,256,handle,request->domain,request->token);
}
static void audout_dispatch(unsigned session,uint8_t *buffer,const IpcRequest *request) {
 audren_capture("output manager",request);
 if(request->command!=1) boundary("audout-manager-command",&state,state.pc);
 if(request->argument_size<16 || request->argument_size>32 || !request->send_pid ||
    request->send_count!=1 || request->receive_count!=1 || request->copy_count!=1 ||
    request->input_object_count || request->send_is_pointer || request->receive_is_pointer ||
    request->receive_mode || request->send_size!=256 || request->receive_size!=256)
  boundary("audout-open-layout",&state,state.pc);
 if(request->copy_handle!=0xffff8001u) boundary("audout-process-handle",&state,state.pc);
 uint64_t aruid;memcpy(&aruid,request->arguments+8,8);
 if(aruid!=GetCurrentProcessId()) boundary("audout-resource-user",&state,state.pc);
 unsigned rate=ipc_u32(request->arguments);uint16_t channels;
 memcpy(&channels,request->arguments+4,2); /* offset 6 is padding, observed 0xcafe */
 if(rate!=48000 || channels!=2) boundary("audout-open-format",&state,state.pc);
 Region input=query(request->send_address),output=query(request->receive_address);
 if(!(input.permission&1) || request->send_address<input.base ||
    request->send_address-input.base>=input.size || 256>input.size-(request->send_address-input.base))
  boundary("audout-name-not-readable",&state,state.pc);
 if(!(output.permission&2) || request->receive_address<output.base ||
    request->receive_address-output.base>=output.size || 256>output.size-(request->receive_address-output.base))
  boundary("audout-name-not-writable",&state,state.pc);
 const char *name=(const char*)(uintptr_t)request->send_address;
 if(!memchr(name,0,256)) boundary("audout-name-termination",&state,state.pc);
 if(*name && strcmp(name,"DeviceOut")) boundary("audout-device-name",&state,state.pc);
 printf("AUDOUT open name=%s rate=%u channels=%u process=0x%x\n",name,rate,channels,request->copy_handle);
 unsigned slot=0;while(slot<16 && audio_outputs[slot].active)slot++;
 if(slot==16) boundary("audout-stream-limit",&state,state.pc);
 MMRESULT result=wave_output_open(&audio_outputs[slot].wave,rate,channels);
 if(result) {printf("AUDOUT open result=%u\n",result);boundary("audout-host-open",&state,state.pc);}
 audio_outputs[slot].active=1;
 memset((void*)(uintptr_t)request->receive_address,0,256);
 memcpy((void*)(uintptr_t)request->receive_address,"DeviceOut",10);
 unsigned object=reply_object(session,buffer,request,SESSION_AUDIO_OUTPUT);
 uint32_t values[4]={rate,channels,2,1}; /* PCM16, initially stopped */
 if(request->domain) {
  sessions[session].object_audio_output[object-1]=slot+1;
  ipc_reply_domain(buffer,256,0,request->token,values,sizeof(values),object);
 } else {
  sessions[session_index(object)].audio_output=slot+1;
  memset(buffer+32,0,32);memcpy(buffer+32,values,sizeof(values));
  ipc_put32(buffer+4,0x8000000c); /* handle plus aligned CMIF and 16-byte payload */
 }
}
