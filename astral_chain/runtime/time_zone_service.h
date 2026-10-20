/* ITimeZoneService. The device time zone ("my rule") is the host's local time
 * zone. Layouts (switchbrew Glue services, CalendarTime 8 bytes and
 * CalendarAdditionalInfo 0x18 bytes) and command 101 ToCalendarTimeWithMyRule.
 * Other commands stop by number. */
#include <time.h>
static void reply_data(uint8_t *buffer,const IpcRequest *request,const void *data,size_t size) {
 int ok=request->domain ? ipc_reply_domain(buffer,256,0,request->token,data,size,0) : ipc_reply_data(buffer,256,data,size,request->token);
 if(!ok) boundary("ipc-reply-data",&state,state.pc);
}
static void time_zone_dispatch(uint8_t *buffer,const IpcRequest *request) {
 if(request->command==101 && request->argument_size>=8) {
  int64_t posix;memcpy(&posix,request->arguments,8);
  __time64_t t=posix;struct tm local;
  if(_localtime64_s(&local,&t)) boundary("timezone-time-range",&state,state.pc);
  TIME_ZONE_INFORMATION zone;GetTimeZoneInformation(&zone);
  int dst=local.tm_isdst>0;
  int32_t offset=-(int32_t)(zone.Bias+(dst ? zone.DaylightBias : zone.StandardBias))*60;
  uint8_t out[32]={0};
  uint16_t year=(uint16_t)(local.tm_year+1900);memcpy(out,&year,2);
  out[2]=(uint8_t)(local.tm_mon+1);out[3]=(uint8_t)local.tm_mday;
  out[4]=(uint8_t)local.tm_hour;out[5]=(uint8_t)local.tm_min;out[6]=(uint8_t)local.tm_sec;
  uint32_t weekday=(uint32_t)local.tm_wday,yearday=(uint32_t)local.tm_yday,is_dst=(uint32_t)dst;
  memcpy(out+8,&weekday,4);memcpy(out+12,&yearday,4);
  /* Abbreviation: Windows offers only long names; report UTC+hh[mm]. */
  char name[9];int hours=offset/3600,minutes=(offset<0?-offset:offset)%3600/60;
  if(minutes)snprintf(name,sizeof(name),"%+03d%02d",hours,minutes);else snprintf(name,sizeof(name),"UTC%+d",hours);
  memcpy(out+16,name,strnlen(name,8));
  memcpy(out+24,&is_dst,4);memcpy(out+28,&offset,4);
  reply_data(buffer,request,out,32);return;
 }
 printf("TIMEZONE command=%u size=%u\n",request->command,(unsigned)request->argument_size);
 boundary("timezone-command",&state,state.pc);
}
