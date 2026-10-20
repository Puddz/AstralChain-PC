/* Window-local key state, separate from the FullKey controller mapping. */
static unsigned keyboard_active;
static uint64_t keyboard_keys[4],keyboard_locks,keyboard_sample,keyboard_tick;
typedef struct { uint64_t sample,modifiers,keys[4]; } KeyboardSample;
typedef struct { uint64_t sequence; KeyboardSample state; } KeyboardSlot;
typedef struct { uint64_t unused,capacity,tail,count; KeyboardSlot slots[17]; } KeyboardRing;
_Static_assert(sizeof(KeyboardSample)==48 && sizeof(KeyboardRing)==984,"keyboard HID ABI");
static unsigned keyboard_usage(unsigned key,LPARAM flags) {
 unsigned extended=((uintptr_t)flags>>24)&1;
 if(key>='A' && key<='Z') return key-'A'+4;
 if(key>='1' && key<='9') return key-'1'+30;
 if(key=='0') return 39;
 if(key>=VK_F1 && key<=VK_F12) return key-VK_F1+58;
 if(key>=VK_F13 && key<=VK_F24) return key-VK_F13+104;
 if(key>=VK_NUMPAD1 && key<=VK_NUMPAD9) return key-VK_NUMPAD1+89;
 switch(key) {
 case VK_SHIFT:return (((uintptr_t)flags>>16)&255)==0x36 ? 229 : 225;
 case VK_CONTROL:return extended ? 228 : 224;
 case VK_MENU:return extended ? 230 : 226;
 case VK_LCONTROL:return 224;case VK_RCONTROL:return 228;
 case VK_LSHIFT:return 225;case VK_RSHIFT:return 229;
 case VK_LMENU:return 226;case VK_RMENU:return 230;
 case VK_LWIN:return 227;case VK_RWIN:return 231;
 case VK_RETURN:return extended ? 88 : 40;
 case VK_ESCAPE:return 41;case VK_BACK:return 42;case VK_TAB:return 43;case VK_SPACE:return 44;
 case VK_OEM_MINUS:return 45;case VK_OEM_PLUS:return 46;case VK_OEM_4:return 47;case VK_OEM_6:return 48;
 case VK_OEM_5:return 49;case VK_OEM_1:return 51;case VK_OEM_7:return 52;case VK_OEM_3:return 53;
 case VK_OEM_COMMA:return 54;case VK_OEM_PERIOD:return 55;case VK_OEM_2:return 56;
 case VK_CAPITAL:return 57;case VK_SNAPSHOT:return 70;case VK_SCROLL:return 71;case VK_PAUSE:return 72;
 case VK_INSERT:return 73;case VK_HOME:return 74;case VK_PRIOR:return 75;case VK_DELETE:return 76;
 case VK_END:return 77;case VK_NEXT:return 78;case VK_RIGHT:return 79;case VK_LEFT:return 80;
 case VK_DOWN:return 81;case VK_UP:return 82;case VK_NUMLOCK:return 83;case VK_DIVIDE:return 84;
 case VK_MULTIPLY:return 85;case VK_SUBTRACT:return 86;case VK_ADD:return 87;
 case VK_NUMPAD0:return 98;case VK_DECIMAL:return 99;case VK_OEM_102:return 100;case VK_APPS:return 101;
 default:return 0;
 }
}
static void keyboard_publish(uint64_t now,int focused) {
 if(!keyboard_active || !hid_page) return;
 KeyboardSample sample={0};sample.sample=++keyboard_sample;
 if(focused) {
  memcpy(sample.keys,keyboard_keys,sizeof(sample.keys));sample.modifiers=keyboard_locks;
  unsigned mods=(unsigned)(keyboard_keys[3]>>32)&255;
  if(mods&0x11) sample.modifiers|=1;
  if(mods&0x22) sample.modifiers|=2;
  if(mods&4) sample.modifiers|=4;
  if(mods&64) sample.modifiers|=8;
  if(mods&0x88) sample.modifiers|=16;
 }
 KeyboardRing *ring=(KeyboardRing*)(hid_page+0x3800);
 unsigned next=(unsigned)((ring->tail+1)%17);ring->slots[next].state=sample;
 InterlockedExchange64((volatile LONG64*)&ring->slots[next].sequence,(LONG64)sample.sample);
 ring->capacity=17;InterlockedExchange64((volatile LONG64*)&ring->tail,next);
 if(ring->count<16) ring->count++;
 keyboard_tick=now;
}
static void keyboard_event(unsigned key,LPARAM flags,int down) {
 unsigned usage=keyboard_usage(key,flags);
 if(usage) {
  uint64_t bit=1ull<<(usage%64);
  if(down) keyboard_keys[usage/64]|=bit;else keyboard_keys[usage/64]&=~bit;
 }
 keyboard_locks=((GetKeyState(VK_CAPITAL)&1) ? 1ull<<8 : 0) |
                ((GetKeyState(VK_SCROLL)&1) ? 1ull<<9 : 0) |
                ((GetKeyState(VK_NUMLOCK)&1) ? 1ull<<10 : 0);
 keyboard_publish(rt_ticks(),1);
}
