/* Observed REV5 device service creation. Renderer/stream commands remain explicit
 * boundaries until their actual contracts and host implementations are provided. */
static void audren_capture(const char *label,const IpcRequest *request) {
 if(!hot_log_on())return;
 printf("AUDREN %s command=%u size=%u pid=%u send=%u receive=%u copy=%u objects=%u arguments=",
  label,request->command,(unsigned)request->argument_size,request->send_pid,request->send_count,
  request->receive_count,request->copy_count,request->input_object_count);
 unsigned captured=request->argument_size<80?(unsigned)request->argument_size:80;
 for(unsigned i=0;i<captured;i++) printf("%02x",request->arguments[i]);
 printf("%s\n",request->argument_size>captured?" (prefix)":"");
}
static void audren_dispatch(unsigned session,uint8_t *buffer,const IpcRequest *request) {
 audren_capture("manager",request);
 if(request->command!=4) boundary("audren-manager-command",&state,state.pc);
 if(request->argument_size<16 || request->argument_size>24 || request->send_pid ||
    request->send_count || request->receive_count || request->copy_count || request->input_object_count)
  boundary("audren-device-layout",&state,state.pc);
 AudioDeviceState device={0};device.revision=ipc_u32(request->arguments);
 memcpy(&device.aruid,request->arguments+8,8);
 if(device.revision!=0x35564552) boundary("audren-device-revision",&state,state.pc);
 if(device.aruid!=GetCurrentProcessId()) boundary("audren-device-resource-user",&state,state.pc);
 unsigned object=reply_object(session,buffer,request,SESSION_AUDIO_DEVICE);
 if(request->domain) sessions[session].object_audio_device[object-1]=device;
 else sessions[session_index(object)].audio_device=device;
}
/* Platform route names are logical interfaces, including disconnected routes.
 * They are not claims that five physical Windows playback endpoints exist.
 * Ordering/names follow the pinned VirtualDevice inventory in native-execution.md. */
static const char audio_device_names[][256]={
 "AudioStereoJackOutput", "AudioBuiltInSpeakerOutput", "AudioTvOutput",
 "AudioUsbDeviceOutput", "AudioExternalOutput"
};
/* One authorized application resource ID per runtime process. Route gain belongs
 * to that application, not to an individual IAudioDevice session. Renderer gain
 * application remains unimplemented; these are retained guest configuration. */
static uint32_t audio_device_volume[5];
static unsigned audio_device_volume_set;
static void audio_device_dispatch(unsigned session,uint8_t *buffer,const IpcRequest *request) {
 const AudioDeviceState *device=request->domain ?
  &sessions[session].object_audio_device[request->object_id-1] : &sessions[session].audio_device;
 if(device->revision!=0x35564552 || device->aruid!=GetCurrentProcessId())
  boundary("audio-device-state",&state,state.pc);
 audren_capture("device",request);
 if(request->command==0 || request->command==6) {
  if(request->argument_size>8 || request->send_pid || request->send_count ||
     request->copy_count || request->input_object_count || request->receive_count!=1 ||
     request->receive_size>1048576 || request->receive_size%256 ||
     (request->command==0 && (request->receive_mode || request->receive_is_pointer)))
   boundary("audio-device-list-layout",&state,state.pc);
  if(request->receive_size) {
   Region output=query(request->receive_address);
   if(!(output.permission&2) || request->receive_address<output.base ||
      request->receive_address-output.base>=output.size ||
      request->receive_size>output.size-(request->receive_address-output.base))
    boundary("audio-device-output-not-writable",&state,state.pc);
  }
  unsigned count=sizeof(audio_device_names)/sizeof(audio_device_names[0]);
  if(count>request->receive_size/256) count=(unsigned)(request->receive_size/256);
  if(count) memcpy((void*)(uintptr_t)request->receive_address,audio_device_names,count*256);
  printf("AUDREN device list count=%u capacity=%llu pointer=%u\n",count,
   (unsigned long long)(request->receive_size/256),request->receive_is_pointer);
  reply_value(buffer,request,count);return;
 }
 if(request->command==1 || request->command==2 || request->command==7 || request->command==8) {
  unsigned setting=request->command==1 || request->command==7;
  if(request->argument_size>(setting?16:8) || (setting && request->argument_size<4) ||
     request->send_pid || request->send_count!=1 || request->receive_count ||
     request->copy_count || request->input_object_count || !request->send_size || request->send_size>256)
   boundary("audio-device-volume-layout",&state,state.pc);
  Region input=query(request->send_address);
  if(!(input.permission&1) || request->send_address<input.base ||
     request->send_address-input.base>=input.size ||
     request->send_size>input.size-(request->send_address-input.base))
   boundary("audio-device-name-not-readable",&state,state.pc);
  const char *name=(const char*)(uintptr_t)request->send_address;
  if(!memchr(name,0,(size_t)request->send_size)) boundary("audio-device-name-termination",&state,state.pc);
  unsigned index=0;
  while(index<5 && strcmp(name,audio_device_names[index])) index++;
  if(index==5) boundary("audio-device-name",&state,state.pc);
  if(setting) {
   uint32_t value=ipc_u32(request->arguments);
   if((value&0x7f800000)==0x7f800000) boundary("audio-device-volume-nonfinite",&state,state.pc);
   audio_device_volume[index]=value;audio_device_volume_set|=1u<<index;
   printf("AUDREN volume name=%s bits=0x%08x\n",name,value);
   reply_result(buffer,request,0);
  } else {
   if(!(audio_device_volume_set&(1u<<index))) boundary("audio-device-volume-uninitialized",&state,state.pc);
   reply_value(buffer,request,audio_device_volume[index]);
  }
  return;
 }
 boundary("audio-device-command",&state,state.pc);
}
