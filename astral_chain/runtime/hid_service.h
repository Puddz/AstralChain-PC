/* Process-local HID with a keyboard-backed FullKey controller in slot 0.
 * Publication runs on the guest thread; no asynchronous guest-memory writer. */
#define HID_MEMORY_SIZE 0x40000
static HANDLE hid_memory;
static uint8_t *hid_page;
static unsigned hid_active,hid_revision,hid_styles,hid_ids=1;
/* Handheld attachment policy is retained even though the desktop backend has
 * no handheld device. It does not change the FullKey controller's connection. */
static uint64_t hid_handheld_mode;
static uint64_t hid_joy_hold_type;
static uint8_t hid_keys[256];
/* Keyboard mapping (the launcher's Controls): AC_KEYS="A:74,B:75,..." with Windows virtual-key codes, 0 for none.
 * Order: the FullKey buttons of bits 0-15, then the stick directions of bits 16-23. */
static const char *const hid_key_names[24]={"A","B","X","Y","LSTICK","RSTICK","L","R","ZL","ZR","PLUS","MINUS",
 "DLEFT","DUP","DRIGHT","DDOWN","LLEFT","LUP","LRIGHT","LDOWN","RLEFT","RUP","RRIGHT","RDOWN"};
static uint8_t hid_key_map[24]={'J','K','I','U','1','2','Q','E','Z','C',VK_RETURN,VK_BACK,VK_LEFT,VK_UP,VK_RIGHT,VK_DOWN,
 'A','W','D','S','F','T','H','G'};
static uint8_t hid_key_mapped[256];
static void hid_keys_load(void) {
 static int loaded;
 if(loaded) return;
 loaded=1;
 const char *e=getenv("AC_KEYS");
 for(const char *p=e;p && *p;) {
  char name[16];int vk=0;
  if(sscanf(p,"%15[^:,]:%d",name,&vk)==2 && vk>=0 && vk<256)
   for(unsigned i=0;i<24;i++) if(!strcmp(name,hid_key_names[i])) hid_key_map[i]=(uint8_t)vk;
  p=strchr(p,',');
  if(p) p++;
 }
 for(unsigned i=0;i<24;i++) if(hid_key_map[i]) hid_key_mapped[hid_key_map[i]]=1;
 if(e && *e) printf("[input] keyboard mapping from AC_KEYS\n");
}
static int hid_key(unsigned i) { return hid_key_map[i] && hid_keys[hid_key_map[i]]; }
static uint64_t hid_last_tick,hid_sampling;
static unsigned hid_touch_active;
static uint64_t hid_touch_last_tick,hid_touch_sampling;
#include "keyboard.h"
#include "mouse.h"
static HANDLE hid_style_events[10];
static uint64_t hid_sensor_started[2];
static unsigned hid_sensor_index(uint32_t handle) {
 unsigned style=handle&255,player=(handle>>8)&255,device=(handle>>16)&255;
 if(handle>>24 || style<3 || style>7 || (player>7 && player!=0x20) ||
    (style<=4 ? device!=2 : style==5 ? device>1 : style==6 ? device!=0 : device!=1))
  boundary("unsupported-sixaxis-sensor-handle",&state,state.pc);
 unsigned slot=player==0x20 ? 8 : player;
 return ((style-3)*9+slot)*2+(device==1);
}
/* Subscription keys for SDK FullKey/Handheld/Joy-Con device handles. A
 * registration may precede a connection; it does not create physical hardware. */
static unsigned hid_vibration_index(uint32_t handle) {
 unsigned style=handle&255,player=(handle>>8)&255,device=(handle>>16)&255;
 if(handle>>24 || style<3 || style>7 || device>1 || (player>7 && player!=0x20) ||
    (style==6 && device!=0) || (style==7 && device!=1))
  boundary("unsupported-vibration-device-handle",&state,state.pc);
 unsigned slot=player==0x20 ? 8 : player;
 return ((style-3)*9+slot)*2+device;
}
typedef struct { uint64_t sample,buttons; int32_t lx,ly,rx,ry; uint32_t attributes,reserved; } HidSample;
static void hid_accept_vibration(const IpcRequest *request) {
 if(!request->send_pid || request->argument_size<32 || request->argument_size>40 ||
    request->send_count || request->receive_count || request->copy_count || request->input_object_count)
  boundary("hid-vibration-layout",&state,state.pc);
 uint64_t aruid;memcpy(&aruid,request->arguments+24,8);
 if(!hid_page || aruid!=GetCurrentProcessId()) boundary("invalid-hid-resource-user",&state,state.pc);
 unsigned handle=ipc_u32(request->arguments);
 float value[4];memcpy(value,request->arguments+4,sizeof(value));
 /* Values are not checked further: there is no actuator to drive, and the console clamps rather than rejects. A
  * negative or non-finite value used to stop the game here (an explosion in the File 01 boss fight, 2026-10-21). */
 static unsigned odd_logged;
 for(unsigned i=0;i<4;i++) {
  unsigned bits=ipc_u32(request->arguments+4+4*i);
  if(((bits&0x7f800000u)==0x7f800000u || value[i]<0) && odd_logged<10) {
   odd_logged++;
   printf("HID vibration handle=0x%x value %u out of range (bits 0x%08x): ignored\n",handle,i,bits);
  }
 }
 /* The current keyboard backend has no physical actuator. Accept the command
  * for a valid virtual device without fabricating a connection or actual output. */
 HOT_LOG("HID vibration handle=0x%x low=%g/%g high=%g/%g (no host actuator)\n",
  handle,value[0],value[1],value[2],value[3]);
}
typedef struct { uint64_t sequence; HidSample state; } HidSampleSlot;
typedef struct { uint64_t unused,capacity,tail,count; HidSampleSlot slots[17]; } HidRing;
_Static_assert(sizeof(HidSample)==0x28 && sizeof(HidRing)==0x350,"HID ABI");
typedef struct { uint64_t sample; uint32_t count,reserved; uint8_t touches[16][40]; } HidTouchSample;
typedef struct { uint64_t sequence; HidTouchSample state; } HidTouchSlot;
typedef struct { uint64_t unused,capacity,tail,count; HidTouchSlot slots[17]; } HidTouchRing;
_Static_assert(sizeof(HidTouchSample)==0x290 && sizeof(HidTouchRing)==0x2c38,"touch HID ABI");
static void hid_touch_publish(uint64_t now) {
 if(!hid_page || !hid_touch_active) return;
 HidTouchRing *ring=(HidTouchRing*)(hid_page+0x400);
 unsigned next=(unsigned)((ring->tail+1)%17);
 /* This desktop backend has no touch device. Publish current no-contact
  * samples, not synthetic finger presses or stale touchscreen data. */
 HidTouchSample sample={0};sample.sample=++hid_touch_sampling;
 ring->slots[next].state=sample;
 InterlockedExchange64((volatile LONG64*)&ring->slots[next].sequence,(LONG64)sample.sample);
 ring->capacity=17;InterlockedExchange64((volatile LONG64*)&ring->tail,next);
 if(ring->count<16) ring->count++;
 hid_touch_last_tick=now;
}
static unsigned hid_slot(unsigned id) {
 if(id<8) return id;
 if(id==0x20) return 8;
 if(id==0x10) return 9;
 boundary("invalid-npad-id",&state,state.pc);
}
static void hid_ring_write(HidRing *ring,const HidSample *sample) {
 unsigned next=(unsigned)((ring->tail+1)%17);
 ring->slots[next].state=*sample;
 InterlockedExchange64((volatile LONG64*)&ring->slots[next].sequence,(LONG64)sample->sample);
 ring->capacity=17;
 InterlockedExchange64((volatile LONG64*)&ring->tail,next);
 if(ring->count<16) ring->count++;
}
/* First connected XInput pad, merged with the keyboard. Positional mapping to
 * the Switch layout: Xbox A/B/X/Y (bottom/right/left/top) -> Switch B/A/Y/X. */
typedef struct {uint16_t buttons;uint8_t lt,rt;int16_t lx,ly,rx,ry;} HidXinputPad;
typedef struct {uint32_t packet;HidXinputPad pad;} HidXinputState;
/* Probing absent pads is slow (~100 ms on a second PC): it ran once per second on the game thread under the kernel
 * lock, stopping every guest thread, the sound's too (sound queue dry every ~1.15 s in play). A background thread
 * looks for a pad; the game thread only reads the one found. */
static DWORD (WINAPI *xinput_get_state)(DWORD,HidXinputState*);
static volatile LONG xinput_pad=-1;
static DWORD WINAPI xinput_probe(void *argument) {
 (void)argument;
 SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
 for(;;) {
  if(xinput_pad<0) for(int i=0;i<4;i++) {HidXinputState s;if(!xinput_get_state(i,&s)) {InterlockedExchange(&xinput_pad,i);break;}}
  Sleep(1000);
 }
 return 0;
}
static void hid_xinput(HidSample *sample) {
 static int loaded;
 if(!loaded) {
  loaded=1;HMODULE x=LoadLibraryExW(L"xinput1_4.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
  if(x) xinput_get_state=(DWORD (WINAPI*)(DWORD,HidXinputState*))GetProcAddress(x,"XInputGetState");
  if(xinput_get_state) {HANDLE t=CreateThread(NULL,0,xinput_probe,NULL,0,NULL);if(t) CloseHandle(t);}
 }
 if(!xinput_get_state) return;
 LONG pad=xinput_pad;
 if(pad<0) return;
 HidXinputState s;if(xinput_get_state((DWORD)pad,&s)){InterlockedCompareExchange(&xinput_pad,-1,pad);return;}
 static const struct {uint16_t xinput;unsigned bit;} map[]={
  {0x1000,1},{0x2000,0},{0x4000,3},{0x8000,2},{0x0040,4},{0x0080,5},{0x0100,6},{0x0200,7},
  {0x0010,10},{0x0020,11},{0x0004,12},{0x0001,13},{0x0008,14},{0x0002,15}};
 for(unsigned i=0;i<sizeof(map)/sizeof(map[0]);i++) if(s.pad.buttons&map[i].xinput) sample->buttons|=1ull<<map[i].bit;
 if(s.pad.lt>30) sample->buttons|=1ull<<8;
 if(s.pad.rt>30) sample->buttons|=1ull<<9;
 /* Sticks: radial dead zone of 8000 (XInput guidance), then full scale. */
 int32_t *out[2][2]={{&sample->lx,&sample->ly},{&sample->rx,&sample->ry}};
 int in[2][2]={{s.pad.lx,s.pad.ly},{s.pad.rx,s.pad.ry}};
 for(unsigned k=0;k<2;k++) {
  long long m=(long long)in[k][0]*in[k][0]+(long long)in[k][1]*in[k][1];
  if(m>8000ll*8000 && !*out[k][0] && !*out[k][1]) {*out[k][0]=in[k][0]<-32767?-32767:in[k][0];*out[k][1]=in[k][1]<-32767?-32767:in[k][1];}
 }
}
/* Test automation: AC_INPUT_SCRIPT="t0-t1:mask;..." holds Switch button mask
 * (bit 0 A ... bit 9 ZR, hex) from t0 to t1 seconds after the first sample.
 * Applies regardless of window focus. */
static uint64_t hid_script_buttons(uint64_t now) {
 static char script[4096];static int loaded;static uint64_t origin;
 if(!loaded) {loaded=1;DWORD n=GetEnvironmentVariableA("AC_INPUT_SCRIPT",script,sizeof(script));if(!n || n>=sizeof(script))script[0]=0;}
 if(!script[0]) return 0;
 if(!origin) origin=now;
 double t=(double)(now-origin)/19200000.0;uint64_t mask=0;
 for(const char *p=script;*p;) {
  double a,b;unsigned long long m;int used=0;
  if(sscanf(p,"%lf-%lf:%llx%n",&a,&b,&m,&used)!=3) break;
  p+=used;
  /* Optional "@period/duty": inside [a,b) held for the first `duty` seconds of every `period`. */
  double period=0,duty=0;int more=0;
  if(*p=='@' && sscanf(p,"@%lf/%lf%n",&period,&duty,&more)==2) p+=more; else period=0;
  if(t>=a && t<b && (period<=0 || fmod(t-a,period)<duty)) mask|=m;
  if(*p==';') p++;
 }
 static uint64_t last=~0ull;
 if(mask!=last) {printf("HID script t=%.2f buttons=0x%llx\n",t,(unsigned long long)mask);last=mask;}
 return mask;
}
static void hid_publish(uint64_t now,int focused) {
 if(!hid_page || !hid_active) return;
 unsigned connected=(hid_styles&1) && (hid_ids&1);
 uint8_t *npad=hid_page+0x9a00;
 unsigned previous=ipc_u32(npad);
 ipc_put32(npad,connected ? 1 : 0);
 ipc_put32(npad+8,connected ? 0 : 2); /* full-key color: OK / no controller */
 ipc_put32(npad+20,2); /* no split Joy-Con colors */
 ipc_put32(npad+0x4188,connected ? 1 : 0);
 ipc_put32(npad+0x419c,connected ? 4 : 0);
 if(previous!=(connected ? 1u : 0u) && hid_style_events[0] && !SetEvent(hid_style_events[0]))
  boundary("hid-style-event",&state,state.pc);
 HidSample sample={0};sample.sample=++hid_sampling;sample.attributes=connected ? 3 : 0;
 if(connected && focused) {
  hid_keys_load();
  for(unsigned i=0;i<16;i++) if(hid_key(i)) sample.buttons|=1ull<<i;
  sample.lx=(hid_key(18)-hid_key(16))*32767;sample.ly=(hid_key(17)-hid_key(19))*32767;
  sample.rx=(hid_key(22)-hid_key(20))*32767;sample.ry=(hid_key(21)-hid_key(23))*32767;
  if(sample.lx && sample.ly) {sample.lx=sample.lx>0 ? 23170 : -23170;sample.ly=sample.ly>0 ? 23170 : -23170;}
  if(sample.rx && sample.ry) {sample.rx=sample.rx>0 ? 23170 : -23170;sample.ry=sample.ry>0 ? 23170 : -23170;}
  hid_xinput(&sample);
  if(sample.lx<0) sample.buttons|=1ull<<16;
  if(sample.ly>0) sample.buttons|=1ull<<17;
  if(sample.lx>0) sample.buttons|=1ull<<18;
  if(sample.ly<0) sample.buttons|=1ull<<19;
  if(sample.rx<0) sample.buttons|=1ull<<20;
  if(sample.ry>0) sample.buttons|=1ull<<21;
  if(sample.rx>0) sample.buttons|=1ull<<22;
  if(sample.ry<0) sample.buttons|=1ull<<23;
 }
 if(connected) {
  uint64_t scripted=hid_script_buttons(now);sample.buttons|=scripted;
  /* Scripted stick directions (bits 16-23) also drive the analog axes. */
  if(scripted&(1ull<<16)) sample.lx=-32767;
  if(scripted&(1ull<<18)) sample.lx=32767;
  if(scripted&(1ull<<17)) sample.ly=32767;
  if(scripted&(1ull<<19)) sample.ly=-32767;
  if(scripted&(1ull<<20)) sample.rx=-32767;  /* right stick (camera) */
  if(scripted&(1ull<<22)) sample.rx=32767;
  if(scripted&(1ull<<21)) sample.ry=32767;
  if(scripted&(1ull<<23)) sample.ry=-32767;
 }
 hid_ring_write((HidRing*)(npad+0x28),&sample);
 hid_ring_write((HidRing*)(npad+0x1408),&sample);
 hid_last_tick=now;
}
static void hid_poll(int focused) {
 if(!hid_active && !hid_touch_active && !keyboard_active && !mouse_active) return;
 uint64_t now=rt_ticks();
 if(hid_active && (!hid_sampling || now-hid_last_tick>=153600)) hid_publish(now,focused); /* up to 125 Hz */
 if(hid_touch_active && (!hid_touch_sampling || now-hid_touch_last_tick>=153600)) hid_touch_publish(now);
 if(keyboard_active && (!keyboard_sample || now-keyboard_tick>=153600)) keyboard_publish(now,focused);
 if(mouse_active && (!mouse_sample || now-mouse_tick>=153600)) mouse_publish(now,focused);
}
static void hid_initialize(void) {
 if(hid_page) return;
 hid_memory=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,HID_MEMORY_SIZE,NULL);
 if(!hid_memory) boundary("host-hid-memory-create",&state,state.pc);
 hid_page=MapViewOfFile(hid_memory,FILE_MAP_ALL_ACCESS,0,0,HID_MEMORY_SIZE);
 if(!hid_page) boundary("host-hid-memory-view",&state,state.pc);
 /* Anonymous section pages are initially zero: no devices or samples yet. */
}
