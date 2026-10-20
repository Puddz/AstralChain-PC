/* Network service objects. Creating an interface does not assert connectivity.
 * Wire reference: libnx services/nifm.c CreateGeneralService (command 5). */
static void nifm_dispatch(unsigned session,int kind,uint8_t *buffer,const IpcRequest *request) {
 printf("NIFM %s command=%u size=%u pid=%u send=%u receive=%u copy=%u objects=%u arguments=",
  kind==SESSION_NIFM_ROOT?"root":"general",request->command,(unsigned)request->argument_size,
  request->send_pid,request->send_count,request->receive_count,request->copy_count,request->input_object_count);
 unsigned captured=request->argument_size<80?(unsigned)request->argument_size:80;
 for(unsigned i=0;i<captured;i++)printf("%02x",request->arguments[i]);
 printf("%s\n",request->argument_size>captured?" (prefix)":"");
 if(kind!=SESSION_NIFM_ROOT) boundary("nifm-general-command",&state,state.pc);
 if(request->command!=5) boundary("nifm-root-command",&state,state.pc);
 if(request->argument_size<8 || request->argument_size>24 || !request->send_pid ||
    request->send_count || request->receive_count || request->copy_count ||
    request->input_object_count || request->receive_mode)
  boundary("nifm-create-layout",&state,state.pc);
 uint64_t reserved;memcpy(&reserved,request->arguments,8);
 if(reserved) boundary("nifm-create-reserved",&state,state.pc);
 reply_object(session,buffer,request,SESSION_NIFM_GENERAL);
}
