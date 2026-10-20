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
static void hid_mouse_event(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
 if(message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL) {
  int32_t *remainder=message==WM_MOUSEWHEEL ? &mouse_wheel_remainder_y : &mouse_wheel_remainder_x;
  int32_t *delta=message==WM_MOUSEWHEEL ? &mouse_wheel_y : &mouse_wheel_x;
  *remainder+=(int16_t)HIWORD(wparam);*delta=*remainder/WHEEL_DELTA;*remainder%=WHEEL_DELTA;
 } else {
  int32_t x=(int16_t)LOWORD(lparam),y=(int16_t)HIWORD(lparam);
  if(mouse_position_valid) {mouse_dx=x-mouse_x;mouse_dy=y-mouse_y;}
  mouse_x=x;mouse_y=y;mouse_position_valid=1;
  mouse_buttons=((wparam&MK_LBUTTON) ? 1 : 0)|((wparam&MK_RBUTTON) ? 2 : 0)|
                ((wparam&MK_MBUTTON) ? 4 : 0)|((wparam&MK_XBUTTON2) ? 8 : 0)|((wparam&MK_XBUTTON1) ? 16 : 0);
  if(mouse_buttons && GetCapture()!=window) SetCapture(window);
  else if(!mouse_buttons && GetCapture()==window) ReleaseCapture();
 }
 mouse_publish(rt_ticks(),1);
}
