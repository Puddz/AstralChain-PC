/* Keyboard and mouse bindings, shared by the HID service (runtime/hid_service.h: what each key presses) and the button
 * prompts (las/button_prompts.c: which key the game's button icons show). Keep the defaults in step with the launcher's
 * ControlsForm (launcher/Launcher.cs), which passes AC_KEYS / AC_KEYS2 only when a binding differs from them.
 *
 * Two bindings per action: AC_KEYS="A:74,B:75,..." (main) and AC_KEYS2="..." (alternative), Windows virtual-key codes,
 * 0 for none. Mouse buttons use their own virtual-key codes (1 left, 2 right, 4 middle, 5 back, 6 forward) and the
 * wheel two unassigned ones (IB_VK_WHEEL_UP, IB_VK_WHEEL_DOWN). */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IB_ACTIONS 24
#define IB_VK_LBUTTON 0x01
#define IB_VK_RBUTTON 0x02
#define IB_VK_MBUTTON 0x04
#define IB_VK_XBUTTON1 0x05
#define IB_VK_XBUTTON2 0x06
#define IB_VK_WHEEL_UP 0x0E
#define IB_VK_WHEEL_DOWN 0x0F

/* Order: the FullKey buttons of bits 0-15, then the stick directions of bits 16-23. */
enum { IB_A, IB_B, IB_X, IB_Y, IB_LSTICK, IB_RSTICK, IB_L, IB_R, IB_ZL, IB_ZR, IB_PLUS, IB_MINUS,
       IB_DLEFT, IB_DUP, IB_DRIGHT, IB_DDOWN, IB_LLEFT, IB_LUP, IB_LRIGHT, IB_LDOWN, IB_RLEFT, IB_RUP, IB_RRIGHT, IB_RDOWN };
static const char *const ib_names[IB_ACTIONS] = {"A","B","X","Y","LSTICK","RSTICK","L","R","ZL","ZR","PLUS","MINUS",
 "DLEFT","DUP","DRIGHT","DDOWN","LLEFT","LUP","LRIGHT","LDOWN","RLEFT","RUP","RRIGHT","RDOWN"};
/* Astral Chain: ZR attack, ZL Legion, left stick press sprint, right stick press re-centre the camera, L Legion action,
 * R dismiss the Legion, B dodge, + IRIS, - pause, D-pad up/down weapons. */
static const uint8_t ib_defaults[2][IB_ACTIONS] = {
 {'J','K','I','U',0x10 /* Shift */,IB_VK_MBUTTON,'Q','E',IB_VK_RBUTTON,IB_VK_LBUTTON,0x0D /* Enter */,0x08 /* Backspace */,
  0x25,0x26,0x27,0x28 /* arrows */,'A','W','D','S','F','T','H','G'},
 {0,0x20 /* Space */,0,0,'1','2',IB_VK_XBUTTON1,IB_VK_XBUTTON2,'Z','C',0x09 /* Tab */,0x1B /* Esc */,
  0,IB_VK_WHEEL_UP,0,IB_VK_WHEEL_DOWN,0,0,0,0,0,0,0,0},
};

/* map[slot][action]: the defaults, then AC_KEYS (slot 0) and AC_KEYS2 (slot 1). Returns 1 if either variable was set. */
static int ib_load(uint8_t map[2][IB_ACTIONS]) {
 memcpy(map, ib_defaults, sizeof ib_defaults);
 int set = 0;
 for (int slot = 0; slot < 2; slot++) {
  const char *e = getenv(slot ? "AC_KEYS2" : "AC_KEYS");
  if (e && *e) set = 1;
  for (const char *p = e; p && *p;) {
   char name[16]; int vk = 0;
   if (sscanf(p, "%15[^:,]:%d", name, &vk) == 2 && vk >= 0 && vk < 256)
    for (unsigned i = 0; i < IB_ACTIONS; i++) if (!strcmp(name, ib_names[i])) map[slot][i] = (uint8_t)vk;
   p = strchr(p, ',');
   if (p) p++;
  }
 }
 return set;
}

static int ib_is_mouse(unsigned vk) {
 return vk == IB_VK_LBUTTON || vk == IB_VK_RBUTTON || (vk >= IB_VK_MBUTTON && vk <= IB_VK_XBUTTON2) ||
        vk == IB_VK_WHEEL_UP || vk == IB_VK_WHEEL_DOWN;
}
