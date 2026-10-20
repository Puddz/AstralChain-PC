/* Software keyboard library applet (applet id 0x11), non-inline mode: answers with a fixed text at once.
 * Character creation asks for the player's name here. The text is AC_PLAYER_NAME (UTF-8), else the game's
 * initial string, else "Akira"; it is cut to the configured maximum length.
 * Inputs: common arguments (32 bytes), then the keyboard configuration (layout of the 0x3e0-byte version below;
 * later versions extend it); optionally a work buffer holding the initial string. Output storage: 0x7d8 bytes,
 * u32 result (0 = accepted) followed by the text, UTF-16 or UTF-8 (UseUtf8). Text-check callbacks, password and
 * multi-line forms stop explicitly. Layout from the swkbd configuration as documented by Ryujinx
 * (SoftwareKeyboardConfig) and libnx (SwkbdConfigCommon). */
typedef __typeof__(applet_storage[0]) AppletStorage;
static void swkbd_applet_start(void) {
 if(!library_applet_created || library_applet_running || library_completed || library_input_count<2)
  boundary("swkbd-input-state",&state,state.pc);
 for(unsigned i=0;i<library_input_count;i++) storage_validate(library_inputs[i]);
 const AppletStorage *common=&applet_storage[library_inputs[0]-1];
 const AppletStorage *config=&applet_storage[library_inputs[1]-1];
 if(common->size!=32 || ipc_u32(common->data)!=1 || ipc_u32(common->data+4)!=32) boundary("swkbd-common-arguments",&state,state.pc);
 if(config->size<0x3e0) boundary("swkbd-config-size",&state,state.pc);
 const uint8_t *k=config->data;
 uint32_t mode=ipc_u32(k),max=ipc_u32(k+0x3ac),min=ipc_u32(k+0x3b0),password=ipc_u32(k+0x3b4),form=ipc_u32(k+0x3b8);
 int utf8=k[0x3bd],text_check=k[0x3d0];
 uint32_t initial_offset=ipc_u32(k+0x3c0),initial_length=ipc_u32(k+0x3c4);
 printf("SWKBD version=0x%x config=%llu mode=%u length=%u..%u password=%u form=%u utf8=%d check=%d initial=%u@%u inputs=%u\n",
  ipc_u32(common->data+8),(unsigned long long)config->size,mode,min,max,password,form,utf8,text_check,initial_length,initial_offset,library_input_count);
 if(text_check) boundary("swkbd-text-check",&state,state.pc);
 if(password || k[0x3bc]) boundary("swkbd-unsupported-form",&state,state.pc);
 if(max==0 || max>500) max=500;
 /* The answer as UTF-16 code units. */
 uint16_t text[501];unsigned n=0;
 char name[256];DWORD got=GetEnvironmentVariableA("AC_PLAYER_NAME",name,sizeof(name));
 if(got && got<sizeof(name)) {
  wchar_t wide[256];int w=MultiByteToWideChar(CP_UTF8,0,name,-1,wide,256);
  for(int i=0;i+1<w && n<max;i++) text[n++]=(uint16_t)wide[i];
 } else if(initial_length && library_input_count>2) {
  const AppletStorage *work=&applet_storage[library_inputs[2]-1];
  if(initial_offset>work->size || (uint64_t)initial_length*2>work->size-initial_offset) boundary("swkbd-initial-string",&state,state.pc);
  for(uint32_t i=0;i<initial_length && n<max;i++) text[n++]=(uint16_t)(work->data[initial_offset+2*i]|work->data[initial_offset+2*i+1]<<8);
 }
 if(!n) {static const char fallback[]="Akira";for(unsigned i=0;fallback[i] && n<max;i++) text[n++]=(uint16_t)fallback[i];}
 if(n<min) boundary("swkbd-text-too-short",&state,state.pc);
 library_applet_running=1;
 if(library_launch_event && !ResetEvent(library_launch_event)) boundary("library-launch-event",&state,state.pc);
 library_output=storage_create(0x7d8);storage_retain(library_output);
 uint8_t *out=applet_storage[library_output-1].data;
 ipc_put32(out,0);
 if(utf8) {
  wchar_t wide[501];for(unsigned i=0;i<n;i++) wide[i]=(wchar_t)text[i];
  WideCharToMultiByte(CP_UTF8,0,wide,(int)n,(char*)out+4,0x7d8-5,NULL,NULL);
 } else {
  for(unsigned i=0;i<n;i++) {out[4+2*i]=(uint8_t)text[i];out[5+2*i]=(uint8_t)(text[i]>>8);}
 }
 if(!library_state_event) library_state_event=CreateEventW(NULL,TRUE,FALSE,NULL);
 if(!library_state_event || !SetEvent(library_state_event)) boundary("library-state-event",&state,state.pc);
 library_applet_running=0;library_completed=1;
 if(library_launch_event && !SetEvent(library_launch_event)) boundary("library-launch-event",&state,state.pc);
 printf("SWKBD accepted %u characters\n",n);
}
