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
/* Held keys by Windows virtual-key code. Mouse buttons and wheel use their codes too (las/input_bindings.h); they are
 * only set while the game has the mouse (mouse.h). */
static uint8_t hid_keys[256];
/* Keyboard and mouse mapping (the launcher's Controls): two bindings per action, las/input_bindings.h. */
#include "las/input_bindings.h"
static uint8_t hid_key_map[2][IB_ACTIONS];
static uint8_t hid_key_mapped[256];
static unsigned hid_mouse_bound;  /* (a mouse button or the wheel is bound: clicking the window takes the mouse) */
static void hid_keys_load(void) {
 static int loaded;
 if(loaded) return;
 loaded=1;
 int custom=ib_load(hid_key_map);
 for(unsigned s=0;s<2;s++) for(unsigned i=0;i<IB_ACTIONS;i++) {
  unsigned vk=hid_key_map[s][i];
  if(vk) hid_key_mapped[vk]=1;
  if(vk && ib_is_mouse(vk)) hid_mouse_bound=1;
 }
 if(custom) printf("[input] key mapping from AC_KEYS / AC_KEYS2\n");
}
static int hid_key(unsigned i) {
 return (hid_key_map[0][i] && hid_keys[hid_key_map[0][i]]) || (hid_key_map[1][i] && hid_keys[hid_key_map[1][i]]);
}
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
/* Every connected XInput pad, merged (buttons of any, each stick from the pad that moves it most): a pad that was
 * connected first need not be the one in use. A Steam Controller's receiver is a pad from the start, even with the
 * controller off or its input taken by Steam Input, whose virtual pad comes later; a pad plugged in after the start
 * also comes later. Reading only the first pad found, the game kept reading the receiver until it was restarted.
 * Probing absent pads is slow (~100 ms on a second PC; it ran once per second on the game thread under the kernel
 * lock, stopping every guest thread, the sound's too): a background thread looks for pads each second, the game
 * thread reads the ones found. */
static DWORD (WINAPI *xinput_get_state)(DWORD,HidXinputState*);
static volatile LONG xinput_pads;  /* bit i: pad i answered */
static DWORD WINAPI xinput_probe(void *argument) {
 (void)argument;
 SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
 for(;;) {
  for(int i=0;i<4;i++) {
   if(xinput_pads&(1<<i)) continue;
   HidXinputState s;
   if(!xinput_get_state(i,&s)) {InterlockedOr(&xinput_pads,1<<i);printf("HID pad %d connected\n",i);fflush(stdout);}
  }
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
 static const struct {uint16_t xinput;unsigned bit;} map[]={
  {0x1000,1},{0x2000,0},{0x4000,3},{0x8000,2},{0x0040,4},{0x0080,5},{0x0100,6},{0x0200,7},
  {0x0010,10},{0x0020,11},{0x0004,12},{0x0001,13},{0x0008,14},{0x0002,15}};
 /* Sticks: radial dead zone of 8000 (XInput guidance), then full scale; the keys' direction wins (already set). */
 int32_t *out[2][2]={{&sample->lx,&sample->ly},{&sample->rx,&sample->ry}};
 int keyed[2]={*out[0][0] || *out[0][1],*out[1][0] || *out[1][1]};
 long long best[2]={8000ll*8000,8000ll*8000};
 LONG pads=xinput_pads;
 for(int pad=0;pad<4;pad++) {
  if(!(pads&(1<<pad))) continue;
  HidXinputState s;
  if(xinput_get_state((DWORD)pad,&s)) {InterlockedAnd(&xinput_pads,~(1<<pad));printf("HID pad %d disconnected\n",pad);fflush(stdout);continue;}
  for(unsigned i=0;i<sizeof(map)/sizeof(map[0]);i++) if(s.pad.buttons&map[i].xinput) sample->buttons|=1ull<<map[i].bit;
  if(s.pad.lt>30) sample->buttons|=1ull<<8;
  if(s.pad.rt>30) sample->buttons|=1ull<<9;
  int in[2][2]={{s.pad.lx,s.pad.ly},{s.pad.rx,s.pad.ry}};
  for(unsigned k=0;k<2;k++) {
   long long m=(long long)in[k][0]*in[k][0]+(long long)in[k][1]*in[k][1];
   if(m>best[k] && !keyed[k]) {
    best[k]=m;
    *out[k][0]=in[k][0]<-32767?-32767:in[k][0];*out[k][1]=in[k][1]<-32767?-32767:in[k][1];
   }
  }
 }
}
/* Other controllers (Switch Pro, DualSense, DualShock 4, 8BitDo, ...) through SDL3's gamepad API: SDL3.dll from
 * tools\sdl3 (AC_SDL3, set by run_guarded.ps1), loaded at run time; without it only XInput pads work. Xbox pads are
 * left to XInput above. SDL runs on its own thread (it opens devices, which can take a while, and wants its updates
 * on one thread); the game thread reads the latest state. SDL's buttons are positional, so south/east/west/north
 * map to the Switch's B/A/Y/X as for XInput, whatever the labels on the pad say. */
typedef struct {uint32_t buttons;int16_t lx,ly,rx,ry,lt,rt;} HidSdlState;
static HidSdlState sdl_state;
static volatile LONG sdl_seq,sdl_have;
static DWORD WINAPI sdl_pad_thread(void *argument) {
 HMODULE m=(HMODULE)argument;
 uint8_t (*init)(uint32_t)=(void*)GetProcAddress(m,"SDL_Init");
 uint8_t (*set_hint)(const char*,const char*)=(void*)GetProcAddress(m,"SDL_SetHint");
 uint32_t *(*get_pads)(int*)=(void*)GetProcAddress(m,"SDL_GetGamepads");
 void *(*open_pad)(uint32_t)=(void*)GetProcAddress(m,"SDL_OpenGamepad");
 void (*close_pad)(void*)=(void*)GetProcAddress(m,"SDL_CloseGamepad");
 uint8_t (*connected)(void*)=(void*)GetProcAddress(m,"SDL_GamepadConnected");
 void (*update)(void)=(void*)GetProcAddress(m,"SDL_UpdateGamepads");
 int (*pad_type)(void*)=(void*)GetProcAddress(m,"SDL_GetGamepadType");
 const char *(*pad_name)(void*)=(void*)GetProcAddress(m,"SDL_GetGamepadName");
 uint8_t (*button)(void*,int)=(void*)GetProcAddress(m,"SDL_GetGamepadButton");
 int16_t (*axis)(void*,int)=(void*)GetProcAddress(m,"SDL_GetGamepadAxis");
 void (*sdl_free)(void*)=(void*)GetProcAddress(m,"SDL_free");
 const char *(*error)(void)=(void*)GetProcAddress(m,"SDL_GetError");
 if(!init || !set_hint || !get_pads || !open_pad || !close_pad || !connected || !update || !pad_type || !pad_name ||
    !button || !axis || !sdl_free || !error) {printf("[input] SDL3: missing functions, controllers through XInput only\n");return 0;}
 SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
 set_hint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS","1"); /* (no SDL window: the game decides when input counts) */
 if(!init(0x00002000u)) {printf("[input] SDL3 gamepads unavailable: %s\n",error());return 0;}
 printf("[input] SDL3 gamepads ready (Xbox pads through XInput)\n");
 /* Switch bit for SDL buttons 0-14: south east west north back guide start lstick rstick lshoulder rshoulder up down
  * left right (guide: none). */
 static const int8_t map[15]={1,0,3,2,11,-1,10,4,5,6,7,13,15,12,14};
 /* (test) AC_SDL_TEST="t0-t1:b,...": a virtual SDL gamepad holding SDL button b (100 + n: axis n at its minimum,
  * 110 + n: at its maximum) from t0 to t1 seconds after SDL starts - the path of a real non-Xbox pad, without one. */
 static char test[512];
 void *vjoy=NULL;
 DWORD tn=GetEnvironmentVariableA("AC_SDL_TEST",test,sizeof(test));
 if(tn && tn<sizeof(test)) {
  struct {uint32_t version;uint16_t type,padding,vendor,product,naxes,nbuttons,nballs,nhats,ntouchpads,nsensors,padding2[2];
          uint32_t button_mask,axis_mask;const char *name;void *rest[11];} desc={0};
  desc.version=sizeof(desc);desc.type=1;desc.naxes=6;desc.nbuttons=15;desc.button_mask=0x7fff;desc.axis_mask=0x3f;desc.name="AC_SDL_TEST virtual gamepad";
  uint32_t (*attach)(const void*)=(void*)GetProcAddress(m,"SDL_AttachVirtualJoystick");
  void *(*open_joy)(uint32_t)=(void*)GetProcAddress(m,"SDL_OpenJoystick");
  uint32_t id=attach ? attach(&desc) : 0;
  vjoy=id && open_joy ? open_joy(id) : NULL;
  printf("[input] AC_SDL_TEST: virtual gamepad %s\n",vjoy ? "attached" : error());
 }
 uint8_t (*vbutton)(void*,int,uint8_t)=(void*)GetProcAddress(m,"SDL_SetJoystickVirtualButton");
 uint8_t (*vaxis)(void*,int,int16_t)=(void*)GetProcAddress(m,"SDL_SetJoystickVirtualAxis");
 uint64_t t0=GetTickCount64();
 uint32_t skipped[8];unsigned nskipped=0;
 void *pad=NULL;
 for(;;) {
  if(vjoy && vbutton && vaxis) {
   double t=(GetTickCount64()-t0)/1000.0;
   uint32_t held=0;int16_t ax[6]={0,0,0,0,-32768,-32768}; /* (a virtual trigger's raw rest is its minimum) */
   for(const char *q=test;*q;) {
    double a,b;int c,used=0;
    if(sscanf(q,"%lf-%lf:%d%n",&a,&b,&c,&used)!=3) break;
    q+=used;if(*q==',') q++;
    if(t<a || t>=b) continue;
    if(c>=0 && c<15) held|=1u<<c;
    else if(c>=100 && c<106) ax[c-100]=-32768;
    else if(c>=110 && c<116) ax[c-110]=32767;
   }
   for(int b=0;b<15;b++) vbutton(vjoy,b,(held>>b)&1);
   for(int k=0;k<6;k++) vaxis(vjoy,k,ax[k]);
  }
  update();
  if(pad && !connected(pad)) {
   printf("[input] controller disconnected\n");close_pad(pad);pad=NULL;InterlockedExchange(&sdl_have,0);
   SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
  }
  if(!pad) {
   int n=0;uint32_t *ids=get_pads(&n);
   for(int i=0;ids && i<n && !pad;i++) {
    unsigned k=0;
    while(k<nskipped && skipped[k]!=ids[i]) k++;
    if(k<nskipped) continue;
    void *p=open_pad(ids[i]);
    if(!p) continue;
    int t=pad_type(p);
    if(t==2 || t==3) {close_pad(p);if(nskipped<8) skipped[nskipped++]=ids[i];continue;} /* Xbox 360 / One: XInput */
    const char *name=pad_name(p);
    printf("[input] controller: %s (SDL type %d)\n",name ? name : "?",t);
    pad=p;
   }
   if(ids) sdl_free(ids);
   /* No controller: look again in half a second, below normal priority. Polling every 4 ms here doubled the game's
    * pauses over 50 ms on a busy CPU (street route, 2026-11-04). */
   if(!pad) {Sleep(500);continue;}
   SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_NORMAL);
  }
  if(pad) {
   HidSdlState s={0};
   for(int b=0;b<15;b++) if(map[b]>=0 && button(pad,b)) s.buttons|=1u<<map[b];
   s.lx=axis(pad,0);s.ly=axis(pad,1);s.rx=axis(pad,2);s.ry=axis(pad,3);s.lt=axis(pad,4);s.rt=axis(pad,5);
   InterlockedIncrement(&sdl_seq);sdl_state=s;InterlockedIncrement(&sdl_seq); /* (odd while writing) */
   InterlockedExchange(&sdl_have,1);
  }
  Sleep(4);
 }
 return 0;
}
static void hid_sdl_start(void) { /* (once the game uses its controller, focused or not: the pad is ready on focus) */
 static int loaded;
 if(loaded) return;
 loaded=1;
 wchar_t path[MAX_PATH];DWORD n=GetEnvironmentVariableW(L"AC_SDL3",path,MAX_PATH);
 HMODULE m=n && n<MAX_PATH ? LoadLibraryExW(path,NULL,LOAD_WITH_ALTERED_SEARCH_PATH) : NULL;
 if(!m) {if(n) printf("[input] SDL3.dll not loaded (error %lu): controllers through XInput only\n",GetLastError());return;}
 HANDLE t=CreateThread(NULL,0,sdl_pad_thread,m,0,NULL);if(t) CloseHandle(t);
}
static void hid_sdl(HidSample *sample) {
 if(!sdl_have) return;
 HidSdlState s;LONG a,b;
 do {a=sdl_seq;MemoryBarrier();s=sdl_state;MemoryBarrier();b=sdl_seq;} while(a!=b || (a&1));
 for(unsigned i=0;i<16;i++) if(s.buttons&(1u<<i)) sample->buttons|=1ull<<i;
 if(s.lt>8000) sample->buttons|=1ull<<8;
 if(s.rt>8000) sample->buttons|=1ull<<9;
 /* Sticks: SDL's y axes point down, the Switch's up; same radial dead zone as XInput. */
 int32_t *out[2][2]={{&sample->lx,&sample->ly},{&sample->rx,&sample->ry}};
 int in[2][2]={{s.lx,s.ly<-32767 ? 32767 : -s.ly},{s.rx,s.ry<-32767 ? 32767 : -s.ry}};
 for(unsigned k=0;k<2;k++) {
  long long m=(long long)in[k][0]*in[k][0]+(long long)in[k][1]*in[k][1];
  if(m>8000ll*8000 && !*out[k][0] && !*out[k][1]) {*out[k][0]=in[k][0]<-32767?-32767:in[k][0];*out[k][1]=in[k][1];}
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
 mouse_wheel_step(now);
 if(connected) hid_sdl_start();
 static int sdl_test=-1; /* (AC_SDL_TEST: the virtual pad counts in unattended runs, whose window has no focus) */
 if(sdl_test<0) sdl_test=GetEnvironmentVariableA("AC_SDL_TEST",NULL,0)!=0;
 if(connected && (focused || sdl_test)) {
  hid_keys_load();
  for(unsigned i=0;i<16;i++) if(hid_key(i)) sample.buttons|=1ull<<i;
  sample.lx=(hid_key(18)-hid_key(16))*32767;sample.ly=(hid_key(17)-hid_key(19))*32767;
  sample.rx=(hid_key(22)-hid_key(20))*32767;sample.ry=(hid_key(21)-hid_key(23))*32767;
  if(sample.lx && sample.ly) {sample.lx=sample.lx>0 ? 23170 : -23170;sample.ly=sample.ly>0 ? 23170 : -23170;}
  if(sample.rx && sample.ry) {sample.rx=sample.rx>0 ? 23170 : -23170;sample.ry=sample.ry>0 ? 23170 : -23170;}
  hid_xinput(&sample);
  hid_sdl(&sample);
  mouse_look_stick(&sample.rx,&sample.ry,now);
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
 }
 {  /* (test) AC_LOG_PAD=1: the controller state the game gets, when it changes */
  static int log=-1;static uint64_t last_buttons;static int32_t last_axes[4];
  if(log<0) log=GetEnvironmentVariableA("AC_LOG_PAD",NULL,0)!=0;
  int32_t axes[4]={sample.lx,sample.ly,sample.rx,sample.ry};
  if(log && (sample.buttons!=last_buttons || memcmp(axes,last_axes,sizeof(axes)))) {
   printf("[pad] buttons=0x%llx left %d,%d right %d,%d\n",(unsigned long long)sample.buttons,axes[0],axes[1],axes[2],axes[3]);
   last_buttons=sample.buttons;memcpy(last_axes,axes,sizeof(axes));
  }
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
