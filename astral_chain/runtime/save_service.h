/* ApplicationFunctions::EnsureSaveData for the configured local PC user. */
#include "save_fs.h"
#ifndef SAVE_ROOT_UNITS
#define SAVE_ROOT_UNITS {0}
#define SAVE_IDENTITY_BYTES {0}
#endif
static SaveStore application_save;
static const wchar_t application_save_root[]=SAVE_ROOT_UNITS;
static const uint8_t application_save_identity[64]=SAVE_IDENTITY_BYTES;
/* AC_DATA_DIR\save replaces the build's save root (portable test bundles). */
static const wchar_t *application_save_path(void) {
 static wchar_t moved[221];DWORD n=GetEnvironmentVariableW(L"AC_DATA_DIR",moved,215);
 return n && n<215 ? wcscat(moved,L"\\save") : application_save_root;
}
static void application_ensure_save(uint8_t *buffer,const IpcRequest *request) {
 if(request->argument_size<16 || request->argument_size>32 || request->send_pid || request->send_count ||
    request->receive_count || request->copy_count || request->input_object_count || request->receive_mode)
  boundary("save-ensure-layout",&state,state.pc);
 if(!application_save_root[0]) boundary("save-not-configured",&state,state.pc);
 if(memcmp(request->arguments,application_save_identity+24,16)) boundary("save-user-not-configured",&state,state.pc);
 DWORD error=save_store_ensure(&application_save,application_save_path(),application_save_identity);
 if(error) {
  printf("SAVE provision failed win32=%lu\n",error);
  boundary("save-host-provision-failed",&state,state.pc);
 }
 printf("SAVE persistent user store ready\n");
 reply_wide_value(buffer,request,0); /* No additional storage needed after provisioning. */
}

static void application_mount_save(unsigned session,uint8_t *buffer,const IpcRequest *request) {
 if(request->argument_size<72 || request->argument_size>88 || request->send_pid || request->send_count ||
    request->receive_count || request->copy_count || request->input_object_count || request->receive_mode)
  boundary("save-mount-layout",&state,state.pc);
 if(!sessions[session].initialized) boundary("filesystem-process-not-registered",&state,state.pc);
 if(!application_save.ready) boundary("save-not-provisioned",&state,state.pc);
 const uint8_t *attr=request->arguments+8;uint64_t program,configured,system;
 memcpy(&program,attr,8);memcpy(&configured,application_save_identity+8,8);memcpy(&system,attr+24,8);
 printf("SAVE mount space=%u application=0x%llx type=%u rank=%u index=%u\n",
  request->arguments[0],(unsigned long long)program,attr[32],attr[33],(unsigned)(attr[34]|attr[35]<<8));
 if(request->arguments[0]!=1 || (program && program!=configured) || system || attr[32]!=1 || attr[33] || attr[34] || attr[35])
  boundary("save-mount-attribute",&state,state.pc);
 if(memcmp(attr+8,application_save_identity+24,16)) boundary("save-mount-user",&state,state.pc);
 const uint8_t zero[24]={0};
 if(memcmp(attr+40,zero,24)) boundary("save-mount-extended-attribute",&state,state.pc);
 /* The sole configured store is immutable for the process lifetime. All objects
  * therefore refer to this same held store, including clones/domain objects. */
 reply_object(session,buffer,request,SESSION_SAVE_FILESYSTEM);
 printf("SAVE mounted persistent user filesystem\n");
}
static void save_filesystem_dispatch(unsigned session,uint8_t *buffer,const IpcRequest *request,const uint8_t *input) {
 if(!application_save.ready) boundary("save-not-provisioned",&state,state.pc);
 printf("SAVE filesystem command=%u size=%u send=%u receive=%u pointer=%u send_size=%llu\n",
  request->command,(unsigned)request->argument_size,request->send_count,request->receive_count,
  request->send_is_pointer,(unsigned long long)request->send_size);
 if(request->send_pid || request->receive_count || request->copy_count || request->input_object_count || request->receive_mode)
  boundary("save-filesystem-layout",&state,state.pc);
 uint32_t result;uint64_t value;int has_value;HANDLE file;
 if(!save_fs_command(&application_save,request,input,&result,&value,&has_value,&file))
  boundary("save-filesystem-command",&state,state.pc);
 if(result) {printf("SAVE result=0x%x\n",result);reply_result(buffer,request,result);return;}
 if(file) {
  unsigned object=reply_object(session,buffer,request,SESSION_SAVE_FILE);
  if(request->domain)sessions[session].object_save_file[object-1]=file;
  else sessions[session_index(object)].save_file=file;
  return;
 }
 if(has_value==2) reply_value(buffer,request,(uint32_t)value);
 else if(has_value) reply_wide_value(buffer,request,value);
 else reply_result(buffer,request,0);
}

static void save_file_release(HANDLE *file) {
 if(*file && !CloseHandle(*file))boundary("save-file-close",&state,state.pc);
 *file=NULL;
}
static void save_file_dispatch(unsigned session,uint8_t *buffer,const IpcRequest *request) {
 HANDLE file=request->domain?sessions[session].object_save_file[request->object_id-1]:sessions[session].save_file;
 if(!file)boundary("save-file-object",&state,state.pc);
 if(request->send_pid || (request->send_count && request->command!=1) || request->copy_count || request->input_object_count ||
    request->receive_mode>1 || (request->receive_mode && request->command!=0))
  boundary("save-file-layout",&state,state.pc);
 DWORD error=0;
 if(request->command==1) { /* Write(u32 option, u64 offset, u64 size, A buffer); option bit 0 flushes */
  if(request->argument_size<24 || request->send_count!=1 || request->send_is_pointer || request->receive_count)
   boundary("save-file-write-layout",&state,state.pc);
  uint32_t option=ipc_u32(request->arguments);uint64_t offset,length;
  memcpy(&offset,request->arguments+8,8);memcpy(&length,request->arguments+16,8);
  if(option&~1u || offset>INT64_MAX || length>64*1024*1024 || length>request->send_size)
   boundary("save-file-write-extent",&state,state.pc);
  Region source=query(request->send_address);
  if(length && (!(source.permission&1) || length>source.size-(request->send_address-source.base)))
   boundary("save-file-write-memory",&state,state.pc);
  error=save_file_write(file,offset,(const void*)(uintptr_t)request->send_address,(uint32_t)length);
  if(!error && option&1 && !FlushFileBuffers(file))error=GetLastError();
  if(!error) {reply_result(buffer,request,0);return;}
 } else if(request->command==2) { /* Flush */
  if(FlushFileBuffers(file)) {reply_result(buffer,request,0);return;}
  error=GetLastError();
 } else if(request->command==3) { /* SetSize(u64) */
  if(request->argument_size<8 || request->receive_count)boundary("save-file-setsize-layout",&state,state.pc);
  uint64_t size;memcpy(&size,request->arguments,8);
  if(size>INT64_MAX)boundary("save-file-setsize-extent",&state,state.pc);
  LARGE_INTEGER end;end.QuadPart=(LONGLONG)size;
  if(SetFilePointerEx(file,end,NULL,FILE_BEGIN) && SetEndOfFile(file)) {reply_result(buffer,request,0);return;}
  error=GetLastError();
 } else if(request->command==4) {
  if(request->argument_size>8 || request->receive_count)boundary("save-file-size-layout",&state,state.pc);
  uint64_t size;error=save_file_size(file,&size);
  if(!error) {reply_wide_value(buffer,request,size);return;}
 } else if(request->command==0) {
  if(request->argument_size<24 || request->argument_size>40 || request->receive_count!=1 || request->receive_is_pointer)
   boundary("save-file-read-layout",&state,state.pc);
  uint32_t option=ipc_u32(request->arguments);uint64_t offset,length;
  memcpy(&offset,request->arguments+8,8);memcpy(&length,request->arguments+16,8);
  if(option || offset>INT64_MAX || length>16*1024*1024 || length>request->receive_size)
   boundary("save-file-read-extent",&state,state.pc);
  Region target=query(request->receive_address);
  if(length && (!(target.permission&2) || length>target.size-(request->receive_address-target.base)))
   boundary("save-file-read-memory",&state,state.pc);
  uint32_t read;error=save_file_read(file,offset,(void*)(uintptr_t)request->receive_address,(uint32_t)length,&read);
  if(!error) {reply_wide_value(buffer,request,read);return;}
 } else boundary("save-file-command",&state,state.pc);
 printf("SAVE file operation win32=%lu\n",error);boundary("save-file-host-error",&state,state.pc);
}
