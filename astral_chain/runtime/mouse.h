/* Client-pixel coordinates; wheel values are detents, preserving partial
 * Windows wheel deltas. Only the application window supplies events. */
static unsigned mouse_active,mouse_buttons,mouse_position_valid;
static int32_t mouse_x,mouse_y,mouse_dx,mouse_dy,mouse_wheel_x,mouse_wheel_y;
static int32_t mouse_wheel_remainder_x,mouse_wheel_remainder_y;
static uint64_t mouse_sample,mouse_tick;
typedef struct { uint64_t sample; int32_t x,y,dx,dy,wheel_x,wheel_y; uint32_t buttons,attributes; } MouseSample;
typedef struct { uint64_t sequence; MouseSample state; } MouseSlot;
typedef struct { uint64_t unused,capacity,tail,count; MouseSlot slots[17]; } MouseRing;
_Static_assert(sizeof(MouseSample)==40 && sizeof(MouseRing)==0x350,"mouse HID ABI");
static void mouse_publish(uint64_t now,int focused) {
 if(!mouse_active || !hid_page) return;
 MouseSample sample={0};sample.sample=++mouse_sample;sample.x=mouse_x;sample.y=mouse_y;
 sample.attributes=3;
 if(focused) {sample.dx=mouse_dx;sample.dy=mouse_dy;sample.wheel_x=mouse_wheel_x;sample.wheel_y=mouse_wheel_y;sample.buttons=mouse_buttons;}
 MouseRing *ring=(MouseRing*)(hid_page+0x3400);unsigned next=(unsigned)((ring->tail+1)%17);
 ring->slots[next].state=sample;InterlockedExchange64((volatile LONG64*)&ring->slots[next].sequence,(LONG64)sample.sample);
 ring->capacity=17;InterlockedExchange64((volatile LONG64*)&ring->tail,next);
 if(ring->count<16) ring->count++;
 mouse_dx=mouse_dy=mouse_wheel_x=mouse_wheel_y=0;mouse_tick=now;
}
static void mouse_clear(void) {
 mouse_buttons=mouse_position_valid=0;mouse_dx=mouse_dy=mouse_wheel_x=mouse_wheel_y=0;
 mouse_wheel_remainder_x=mouse_wheel_remainder_y=0;mouse_publish(rt_ticks(),0);
}
/* Mouse play (PC controls). A click in the game window takes the mouse: the cursor is hidden and kept in the window,
 * its movement turns the camera (the right stick, from raw input) and its buttons and wheel press what they are bound
 * to (las/input_bindings.h). The mouse is given back when the window loses focus or is minimized, and while Alt is
 * held (unless Alt is a game key); a click takes it again.
 *   AC_MOUSE_LOOK=0     buttons only, the camera stays on the keys / pad
 *   AC_MOUSE_SENS=<n>   camera sensitivity in percent (default 100)
 *   AC_MOUSE_INVERT=1   moving the mouse up looks down */
static int mouse_captured,mouse_alt_free,mouse_look_on=-1,mouse_invert;
static unsigned mouse_swallow;  /* (buttons held when the mouse was taken: not game input until released) */
static double mouse_sens=1.0,mouse_acc_x,mouse_acc_y,mouse_vel_x,mouse_vel_y;
static uint64_t mouse_look_tick,mouse_wheel_until[2];
static HWND mouse_window;
static void mouse_config(void) {
 if(mouse_look_on>=0) return;
 const char *e=getenv("AC_MOUSE_LOOK");mouse_look_on=!(e && *e=='0');
 e=getenv("AC_MOUSE_SENS");
 if(e && *e) {double s=atof(e)/100.0;if(s>=0.05 && s<=20) mouse_sens=s;}
 e=getenv("AC_MOUSE_INVERT");mouse_invert=e && *e=='1';
}
static void mouse_clip(void) {
 if(!mouse_captured || !mouse_window) return;
 RECT r;GetClientRect(mouse_window,&r);MapWindowPoints(mouse_window,NULL,(POINT*)&r,2);ClipCursor(&r);
}
static void mouse_keys_clear(void) {
 static const uint8_t vks[]={IB_VK_LBUTTON,IB_VK_RBUTTON,IB_VK_MBUTTON,IB_VK_XBUTTON1,IB_VK_XBUTTON2,IB_VK_WHEEL_UP,IB_VK_WHEEL_DOWN};
 for(unsigned i=0;i<sizeof(vks);i++) hid_keys[vks[i]]=0;
 mouse_wheel_until[0]=mouse_wheel_until[1]=0;
}
static void mouse_set_captured(HWND window,int on) {
 if(on==mouse_captured) return;
 mouse_captured=on;mouse_window=window;
 mouse_acc_x=mouse_acc_y=mouse_vel_x=mouse_vel_y=0;mouse_look_tick=0;
 mouse_keys_clear();
 if(on) {
  static int registered;
  if(!registered) {
   RAWINPUTDEVICE device={1,2,0,window};  /* (generic desktop, mouse: WM_INPUT with raw movement) */
   registered=RegisterRawInputDevices(&device,1,sizeof(device)) ? 1 : -1;
   if(registered<0) printf("[input] raw mouse input unavailable (%lu): no mouse camera\n",GetLastError());
  }
  mouse_clip();SetCursor(NULL);
 } else {
  ClipCursor(NULL);
  POINT p;GetCursorPos(&p);SetCursorPos(p.x,p.y);  /* (shows the cursor again: WM_SETCURSOR) */
 }
 printf("[input] mouse %s\n",on ? "taken by the game" : "released");
}
/* Raw movement while the game has the mouse (WM_INPUT). */
static void mouse_raw_input(LPARAM lparam) {
 if(!mouse_captured) return;
 RAWINPUT raw;UINT size=sizeof(raw);
 if(GetRawInputData((HRAWINPUT)lparam,RID_INPUT,&raw,&size,sizeof(RAWINPUTHEADER))==(UINT)-1) return;
 if(raw.header.dwType!=RIM_TYPEMOUSE || (raw.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)) return;
 mouse_acc_x+=raw.data.mouse.lLastX;mouse_acc_y+=raw.data.mouse.lLastY;
}
/* The wheel presses its binding for 0.12 s per notch. */
static void mouse_wheel_expire(uint64_t now) {
 for(unsigned i=0;i<2;i++) if(mouse_wheel_until[i] && now>=mouse_wheel_until[i]) {
  mouse_wheel_until[i]=0;hid_keys[i ? IB_VK_WHEEL_DOWN : IB_VK_WHEEL_UP]=0;
 }
}
/* Mouse movement -> right stick: speed (counts per second) to deflection, full tilt at 1500 counts/s at 100%. The game's
 * stick dead zone would swallow slow movement, so any movement starts at 20% tilt. Keys or a pad already turning the
 * camera win. */
static void mouse_look_stick(int32_t *rx,int32_t *ry,uint64_t now) {
 mouse_config();
 if(!mouse_captured || !mouse_look_on) {mouse_acc_x=mouse_acc_y=0;mouse_look_tick=now;return;}
 double dt=mouse_look_tick ? (double)(now-mouse_look_tick)/19200000.0 : 1/60.0;
 mouse_look_tick=now;
 if(dt<0.002) dt=0.002;
 if(dt>0.1) dt=0.1;
 if(mouse_acc_x==0 && mouse_acc_y==0) {mouse_vel_x=mouse_vel_y=0;return;}
 /* (light smoothing: the game reads the stick once a frame, the mouse reports in uneven bursts) */
 mouse_vel_x=0.5*mouse_vel_x+0.5*mouse_acc_x/dt;mouse_vel_y=0.5*mouse_vel_y+0.5*mouse_acc_y/dt;
 mouse_acc_x=mouse_acc_y=0;
 double x=mouse_vel_x*mouse_sens/1500.0,y=-mouse_vel_y*mouse_sens/1500.0;
 if(mouse_invert) y=-y;
 double m=sqrt(x*x+y*y);
 if(m<1e-6 || *rx || *ry) return;
 double out=0.2+0.8*(m>1 ? 1 : m);
 *rx=(int32_t)(x/m*out*32767);*ry=(int32_t)(y/m*out*32767);
}
static void hid_mouse_event(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
 hid_keys_load();mouse_config();
 if(!mouse_captured && message==WM_LBUTTONDOWN && (hid_mouse_bound || mouse_look_on)) {
  mouse_set_captured(window,1);mouse_swallow=1;  /* (this click only takes the mouse) */
 }
 if(message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL) {
  int32_t *remainder=message==WM_MOUSEWHEEL ? &mouse_wheel_remainder_y : &mouse_wheel_remainder_x;
  int32_t *delta=message==WM_MOUSEWHEEL ? &mouse_wheel_y : &mouse_wheel_x;
  int16_t notch=(int16_t)HIWORD(wparam);
  *remainder+=notch;*delta=*remainder/WHEEL_DELTA;*remainder%=WHEEL_DELTA;
  if(mouse_captured && message==WM_MOUSEWHEEL && notch) {
   unsigned down=notch<0;
   hid_keys[down ? IB_VK_WHEEL_DOWN : IB_VK_WHEEL_UP]=1;mouse_wheel_until[down]=rt_ticks()+2304000;  /* 0.12 s */
  }
 } else {
  int32_t x=(int16_t)LOWORD(lparam),y=(int16_t)HIWORD(lparam);
  if(mouse_position_valid) {mouse_dx=x-mouse_x;mouse_dy=y-mouse_y;}
  mouse_x=x;mouse_y=y;mouse_position_valid=1;
  mouse_buttons=((wparam&MK_LBUTTON) ? 1 : 0)|((wparam&MK_RBUTTON) ? 2 : 0)|
                ((wparam&MK_MBUTTON) ? 4 : 0)|((wparam&MK_XBUTTON2) ? 8 : 0)|((wparam&MK_XBUTTON1) ? 16 : 0);
  if(mouse_buttons && GetCapture()!=window) SetCapture(window);
  else if(!mouse_buttons && GetCapture()==window) ReleaseCapture();
  if(mouse_captured) {
   mouse_swallow&=mouse_buttons;
   unsigned held=mouse_buttons&~mouse_swallow;
   hid_keys[IB_VK_LBUTTON]=(held&1)!=0;hid_keys[IB_VK_RBUTTON]=(held&2)!=0;hid_keys[IB_VK_MBUTTON]=(held&4)!=0;
   hid_keys[IB_VK_XBUTTON1]=(held&16)!=0;hid_keys[IB_VK_XBUTTON2]=(held&8)!=0;
  }
 }
 mouse_publish(rt_ticks(),1);
}
