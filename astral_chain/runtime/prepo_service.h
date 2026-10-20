/* prepo:u play reports. Reports are telemetry for Nintendo's servers and have
 * no effect on the game; this offline PC runtime accepts and discards them.
 * Command identities: switchbrew Prepo_services. Other commands stop. */
static uint64_t prepo_reports_discarded;
static void prepo_dispatch(uint8_t *buffer,const IpcRequest *request) {
 unsigned command=request->command;
 if(command>=10100 && command<=10105) {
  /* SaveReport / SaveReportWithUser variants: event id, report buffer, user. */
  prepo_reports_discarded++;
  if(prepo_reports_discarded<=4 || !(prepo_reports_discarded&255))
   printf("PREPO report discarded command=%u count=%llu\n",command,(unsigned long long)prepo_reports_discarded);
  reply_result(buffer,request,0);return;
 }
 if(command==10200) {reply_result(buffer,request,0);return;}     /* RequestImmediateTransmission */
 if(command==10300) {reply_value(buffer,request,0);return;}      /* GetTransmissionStatus: idle */
 printf("PREPO command=%u size=%u\n",command,(unsigned)request->argument_size);
 boundary("prepo-command",&state,state.pc);
}
