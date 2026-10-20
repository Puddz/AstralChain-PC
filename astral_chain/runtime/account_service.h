/* Application account interface bound to the runtime's sole process. The local
 * PC profile is persisted by the launcher; it is not a Nintendo account. */
#ifndef ACCOUNT_UID_BYTES
#define ACCOUNT_UID_BYTES {0}
#endif
#ifndef ACCOUNT_SWITCH_LOCK
#define ACCOUNT_SWITCH_LOCK -1
#endif
static const uint8_t account_uid[16]=ACCOUNT_UID_BYTES;
static unsigned account_preselected_popped;
static void account_reply_bool(uint8_t *buffer,const IpcRequest *request,uint8_t value) {
 if(request->domain) ipc_reply_domain(buffer,256,0,request->token,&value,1,0);
 else ipc_reply_u32(buffer,256,value,request->token);
}
static void account_manager_dispatch(unsigned session,uint8_t *buffer,const IpcRequest *request) {
 (void)buffer;
 const uint8_t *uid=request->domain ? sessions[session].object_account_user[request->object_id-1] : sessions[session].account_user;
 const uint8_t zero[16]={0};
 if(!memcmp(uid,zero,16) || memcmp(uid,account_uid,16)) boundary("account-manager-user",&state,state.pc);
 printf("ACCOUNT manager command=%u size=%u pid=%u send=%u receive=%u copy=%u objects=%u\n",
  request->command,(unsigned)request->argument_size,request->send_pid,request->send_count,
  request->receive_count,request->copy_count,request->input_object_count);
 boundary("account-manager-command",&state,state.pc);
}
static void account_launch_parameter(unsigned session,uint8_t *buffer,const IpcRequest *request) {
 if(request->argument_size<4 || request->argument_size>16 || request->send_pid || request->send_count ||
    request->receive_count || request->copy_count || request->input_object_count || request->receive_mode)
  boundary("account-launch-layout",&state,state.pc);
 unsigned kind=ipc_u32(request->arguments);printf("ACCOUNT launch parameter kind=%u\n",kind);
 if(kind!=2) boundary("account-launch-kind",&state,state.pc);
 if(account_preselected_popped) {reply_result(buffer,request,0x480);return;} /* AM no data */
 const uint8_t zero[16]={0};
 if(!memcmp(account_uid,zero,16)) boundary("account-profile-not-configured",&state,state.pc);
 unsigned storage=storage_create(0x88);uint8_t *data=applet_storage[storage-1].data;
 ipc_put32(data,0xc79497ca);data[4]=1;memcpy(data+8,account_uid,16);
 reply_storage(session,buffer,request,SESSION_STORAGE,storage);
 account_preselected_popped=1;
}
static void account_dispatch(unsigned session,uint8_t *buffer,const IpcRequest *request) {
 printf("ACCOUNT command=%u size=%u pid=%u send=%u receive=%u copy=%u objects=%u\n",
  request->command,(unsigned)request->argument_size,request->send_pid,request->send_count,
  request->receive_count,request->copy_count,request->input_object_count);
 uint32_t *pid=request->domain ? &sessions[session].object_account_pid[request->object_id-1] : &sessions[session].account_pid;
 if(request->command==1) { /* GetUserExistence */
  if(request->argument_size<16 || request->argument_size>32 || request->send_pid || request->send_count ||
     request->receive_count || request->copy_count || request->input_object_count || request->receive_mode)
   boundary("account-existence-layout",&state,state.pc);
  if(*pid!=GetCurrentProcessId()) boundary("account-not-initialized",&state,state.pc);
  const uint8_t zero[16]={0};
  if(!memcmp(account_uid,zero,16)) boundary("account-profile-not-configured",&state,state.pc);
  if(!memcmp(request->arguments,zero,16)) {reply_result(buffer,request,0x287c);return;}
  uint8_t exists=!memcmp(request->arguments,account_uid,16);
  printf("ACCOUNT user exists=%u\n",exists);
  account_reply_bool(buffer,request,exists);return;
 }
 if(request->command==3) { /* ListOpenUsers: launcher-selected local user. */
  if(request->argument_size>8 || request->send_pid || request->send_count || request->receive_count!=1 ||
     !request->receive_is_pointer || request->receive_mode || request->copy_count || request->input_object_count ||
     request->receive_size>4096 || request->receive_size%16)
   boundary("account-open-users-layout",&state,state.pc);
  if(*pid!=GetCurrentProcessId()) boundary("account-not-initialized",&state,state.pc);
  const uint8_t zero[16]={0};
  if(!memcmp(account_uid,zero,16)) boundary("account-profile-not-configured",&state,state.pc);
  if(request->receive_size) {
   Region output=query(request->receive_address);
   if(!(output.permission&2) || request->receive_address<output.base ||
      request->receive_address-output.base>=output.size ||
      request->receive_size>output.size-(request->receive_address-output.base))
    boundary("account-open-users-memory",&state,state.pc);
   memset((void*)(uintptr_t)request->receive_address,0,(size_t)request->receive_size);
   memcpy((void*)(uintptr_t)request->receive_address,account_uid,16);
  }
  printf("ACCOUNT open users=%u capacity=%llu\n",request->receive_size?1:0,(unsigned long long)(request->receive_size/16));
  reply_result(buffer,request,0);return;
 }
 if(request->command==101) {
  if(request->argument_size<16 || request->argument_size>32 || request->send_pid || request->send_count ||
     request->receive_count || request->copy_count || request->input_object_count || request->receive_mode)
   boundary("account-manager-layout",&state,state.pc);
  if(*pid!=GetCurrentProcessId()) boundary("account-not-initialized",&state,state.pc);
  const uint8_t zero[16]={0};
  if(!memcmp(account_uid,zero,16)) boundary("account-profile-not-configured",&state,state.pc);
  if(!memcmp(request->arguments,zero,16)) {reply_result(buffer,request,0x287c);return;} /* account null argument */
  if(memcmp(request->arguments,account_uid,16)) boundary("account-user-not-configured",&state,state.pc);
  unsigned object=reply_object(session,buffer,request,SESSION_ACCOUNT_MANAGER);
  uint8_t *user=request->domain ? sessions[session].object_account_user[object-1] : sessions[session_index(object)].account_user;
  memcpy(user,account_uid,16);
  puts("ACCOUNT local user manager created");return;
 }
 if(request->command==150) {
  if(request->argument_size>8 || request->send_pid || request->send_count || request->receive_count ||
     request->copy_count || request->input_object_count || request->receive_mode)
   boundary("account-switch-layout",&state,state.pc);
  if(*pid!=GetCurrentProcessId()) boundary("account-not-initialized",&state,state.pc);
  if(ACCOUNT_SWITCH_LOCK!=0 && ACCOUNT_SWITCH_LOCK!=1) boundary("account-control-missing",&state,state.pc);
  uint8_t locked=ACCOUNT_SWITCH_LOCK;
  printf("ACCOUNT switch locked=%u (NACP)\n",locked);
  account_reply_bool(buffer,request,locked);
  return;
 }
 if(request->command!=100 && request->command!=140) boundary("account-command",&state,state.pc);
 if(request->argument_size<8 || request->argument_size>24 || !request->send_pid || request->send_count ||
    request->receive_count || request->copy_count || request->input_object_count || request->receive_mode)
  boundary("account-initialize-layout",&state,state.pc);
 uint64_t placeholder;memcpy(&placeholder,request->arguments,8);
 if(placeholder) boundary("account-process-placeholder",&state,state.pc);
 if(*pid) boundary("account-already-initialized",&state,state.pc);
 *pid=GetCurrentProcessId();
 printf("ACCOUNT application bound pid=%u\n",*pid);
 reply_result(buffer,request,0);
}
