/* Legacy single-stream work-memory contract. This sizes guest transfer memory,
 * not a host libopus decoder. Reference: pinned Ryujinx manager, documented in
 * native-execution.md. No decoding or synthetic PCM is supplied here. */
static unsigned hwopus_work_size(unsigned rate,unsigned channels) {
 if((channels!=1 && channels!=2) ||
    (rate!=8000 && rate!=12000 && rate!=16000 && rate!=24000 && rate!=48000)) return 0;
 unsigned allocation=(channels*0x800+0x4803)&~0x7ffu;
 unsigned celt=(0x2030+120*4)*channels+21*16+0x50;
 unsigned frame=(channels*1920/(48000/rate)+63)&~63u;
 return (allocation|0x4c)+0x2160+celt+1536+frame;
}
static void hwopus_dispatch(uint8_t *buffer,const IpcRequest *request) {
 if(request->command!=1) boundary("hwopus-command",&state,state.pc);
 if(request->argument_size<8 || request->argument_size>16 || request->send_pid ||
    request->send_count || request->receive_count || request->copy_count || request->input_object_count)
  boundary("hwopus-size-layout",&state,state.pc);
 unsigned rate=ipc_u32(request->arguments),channels=ipc_u32(request->arguments+4);
 unsigned size=hwopus_work_size(rate,channels);
 printf("HWOPUS work size rate=%u channels=%u size=0x%x\n",rate,channels,size);
 if(!size) boundary("hwopus-size-parameters",&state,state.pc);
 reply_value(buffer,request,size);
}
