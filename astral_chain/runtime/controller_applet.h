/* Version 5 controller support: retain an already connected FullKey device.
 * Requests needing selection, reconnection or extra UI stop explicitly. */
static void controller_applet_start(void) {
 if(!library_applet_created || library_applet_running || library_completed || library_input_count!=3)
  boundary("controller-applet-input-state",&state,state.pc);
 static const uint64_t sizes[]={32,20,540};
 for(unsigned i=0;i<3;i++) {
  storage_validate(library_inputs[i]);
  if(applet_storage[library_inputs[i]-1].size!=sizes[i]) boundary("controller-applet-input-size",&state,state.pc);
 }
 const uint8_t *common=applet_storage[library_inputs[0]-1].data;
 const uint8_t *priv=applet_storage[library_inputs[1]-1].data;
 const uint8_t *arg=applet_storage[library_inputs[2]-1].data;
 if(ipc_u32(common)!=1 || ipc_u32(common+4)!=32 || ipc_u32(common+8)!=5)
  boundary("unsupported-controller-applet-version",&state,state.pc);
 if(ipc_u32(priv)!=20 || ipc_u32(priv+4)!=540 || ipc_u32(priv+8)!=0)
  boundary("unsupported-controller-applet-mode",&state,state.pc);
 if(ipc_u32(priv+12)!=hid_styles || ipc_u32(priv+16)!=hid_joy_hold_type)
  boundary("controller-applet-policy-mismatch",&state,state.pc);
 if(arg[0]>4 || !arg[1] || arg[1]>4 || arg[0]>arg[1])
  boundary("invalid-controller-player-count",&state,state.pc);
 /* Disabled color/text fields and structure padding are not initialized by
  * all clients. Never interpret them unless their enable flag is set. */
 if(!arg[2] || arg[6] || arg[23] || common[16])
  boundary("controller-applet-ui-required",&state,state.pc);
 if((!arg[5] && arg[0]>1) || !hid_active || !hid_page || !(hid_styles&1) || !(hid_ids&1) || ipc_u32(hid_page+0x9a00)!=1)
  boundary("controller-applet-selection-required",&state,state.pc);
 library_applet_running=1;
 if(library_launch_event && !ResetEvent(library_launch_event)) boundary("library-launch-event",&state,state.pc);
 library_output=storage_create(12);storage_retain(library_output);
 /* One connected player, Npad ID 0, successful controller-support result. */
 applet_storage[library_output-1].data[0]=1;
 if(!library_state_event) library_state_event=CreateEventW(NULL,TRUE,FALSE,NULL);
 if(!library_state_event || !SetEvent(library_state_event)) boundary("library-state-event",&state,state.pc);
 library_applet_running=0;library_completed=1;
 if(library_launch_event && !SetEvent(library_launch_event)) boundary("library-launch-event",&state,state.pc);
 puts("CONTROLLER retained keyboard FullKey: players=1 selected-id=0");
}
