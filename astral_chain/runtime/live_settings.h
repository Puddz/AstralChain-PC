/* In-game settings menu: the runtime's half. The menu itself (las/menu.cpp, Dear ImGui) runs on the render thread,
 * drawn over the game picture; F1 or Minus+Plus on a controller opens and closes it (app_window.h, hid_service.h).
 * While it is open the game gets no input.
 *
 * The runtime never calls into the menu (the diagnostic null backend has none): it publishes window input through a
 * ring and the controller through a few words, and the menu reads them; the menu applies a change through
 * ac_setting_apply. Settings the runtime reads once at startup are made live here; the others apply at the next start
 * (the menu says so). */
volatile LONG ac_menu_open;                    /* 1 while the menu is up */
volatile LONG ac_menu_close_request;           /* the menu asks to close (its button, B): app_pump closes it */
volatile LONG ac_menu_quit_request;            /* Quit game, confirmed: app_pump closes the window, as Alt+F4 does */
volatile LONG ac_menu_modal;                   /* 1 while the menu asks whether to quit: Esc answers it, not the menu */
typedef struct { int kind,a,b; } AcMenuEvent;  /* kind 1 mouse move (x, y), 2 button (0 left 1 right 2 middle, down),
                                                * 3 wheel (notches * 120, horizontal), 4 key (virtual key, down),
                                                * 5 character (UTF-16 unit) */
AcMenuEvent ac_menu_events[256];
volatile LONG ac_menu_head,ac_menu_tail;       /* (written by the window thread, read by the render thread) */
volatile LONG ac_menu_pad_buttons,ac_menu_pad_lx,ac_menu_pad_ly;  /* the controllers while the menu is up */
volatile int ac_prompt_style=-1;  /* AC_PROMPTS: 0 auto (default), 1 keyboard and mouse, 2 Switch, 3 Xbox, 4 PlayStation */
volatile int ac_prompt_device;    /* last used (auto icons): 0 keyboard/mouse, 1 Xbox pad, 2 Switch pad, 3 PlayStation pad */
volatile int ac_mouse_look=-1;    /* AC_MOUSE_LOOK (button prompts show a mouse for the camera) */
volatile int ac_cutscene_30=-1;   /* AC_CUTSCENE_FPS=30, from the next cutscene on */
volatile int ac_ignore_focus=-1;  /* AC_IGNORE_FOCUS=1 */
volatile int ac_upscaler=-1;      /* AC_UPSCALER=fsr1 (las/nvn/vulkan/exec.c post_process) */
volatile float ac_fsr_sharpness=-1;  /* AC_FSR_SHARPNESS */
volatile int ac_uncapped=-1;      /* AC_UNCAPPED=1: no 60 FPS pacing in gameplay (nvn_hle.h nvn_present_pace) */
volatile int ac_in_cutscene;      /* (svc #0xB1: the game reports its cutscene mode each frame) */
volatile LONG ac_game_steps;      /* (svc #0xB1 calls: game frames, for input recordings, input_record.h) */
volatile int ac_fps_counter=-1;  /* AC_FPS_COUNTER=1: frames per second in the corner, 2: the detailed panel (las/menu.cpp) */
volatile int ac_title_credit=-1;  /* "Port by Puddz" on the title screen (las/menu.cpp); AC_TITLE_CREDIT=0: off */
volatile int ac_title_own_twin=-1;  /* AC_TITLE_OWN_TWIN=1: the title shows the player's twin (svc #0xB4) */
volatile int ac_hud_edges=-1;    /* AC_HUD_EDGES=1: wider than 16:9, the HUD's side panels at the screen's edges (exec.c) */
int ac_hud_edges_on(void) {
 if(ac_hud_edges<0) {char v[4];DWORD n=GetEnvironmentVariableA("AC_HUD_EDGES",v,sizeof(v));ac_hud_edges=n==1 && v[0]=='1';}
 return ac_hud_edges;
}
volatile int ac_swap_ab=-1;      /* AC_SWAP_AB=1: the controllers' A and B swapped (hid_service.h, las/button_prompts.c) */

/* Swap A and B: the bottom face button (Xbox A, PlayStation cross) is the Switch's A, which confirms and talks, and the
 * right one its B. By default the buttons go by position, as on the Switch, where A is on the right. */
int ac_swap_ab_on(void) {
 if(ac_swap_ab<0) {char v[4];DWORD n=GetEnvironmentVariableA("AC_SWAP_AB",v,sizeof(v));ac_swap_ab=n==1 && v[0]=='1';}
 return ac_swap_ab;
}

int ac_prompt_parse(const char *v) {
 if(!v || !*v || !strcmp(v,"auto")) return 0;
 if(!strcmp(v,"keyboard")) return 1;
 if(!strcmp(v,"switch") || !strcmp(v,"0")) return 2;
 if(!strcmp(v,"xbox")) return 3;
 if(!strcmp(v,"playstation")) return 4;
 return 0;
}
static void menu_set_open(int open);  /* (app_window.h) */
static void menu_push(int kind,int a,int b) {
 LONG head=ac_menu_head;
 if(head-ac_menu_tail>=256) return;  /* (full: the menu is not drawing) */
 ac_menu_events[head&255]=(AcMenuEvent){kind,a,b};
 MemoryBarrier();
 InterlockedExchange(&ac_menu_head,head+1);
}
