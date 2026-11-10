/* Native application lifecycle. Desktop input focus is distinct from the
 * guest applet's active state: minimize backgrounds it; ordinary alt-tab does not. */
static HWND app_window;
static uint8_t *app_frame_pixels;
static unsigned app_frame_width,app_frame_height;
static uint64_t app_frame_tick,app_frame_count;
static int (WINAPI *app_stretch_dib)(HDC,int,int,int,int,int,int,int,int,const void *,const BITMAPINFO *,UINT,DWORD);
static void app_paint_frame(HDC dc,const RECT *client) {
 if(!app_frame_pixels) return;
 BITMAPINFO info={0};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
 info.bmiHeader.biWidth=app_frame_width;info.bmiHeader.biHeight=-(LONG)app_frame_height;
 info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
 if(app_stretch_dib(dc,0,0,client->right-client->left,client->bottom-client->top,0,0,
     app_frame_width,app_frame_height,app_frame_pixels,&info,DIB_RGB_COLORS,SRCCOPY)==GDI_ERROR)
  boundary("host-display-paint",&state,state.pc);
}
static void app_snapshot_frame(const char *path) {
 if(!app_frame_pixels) return;
 char name[1024];if(snprintf(name,sizeof(name),"%s.frame.bmp",path)>=(int)sizeof(name)) {fputs("Display snapshot path too long\n",stderr);ExitProcess(33);}
 FILE *file=fopen(name,"wb");if(!file) {fputs("Display snapshot open failed\n",stderr);ExitProcess(33);}
 BITMAPFILEHEADER file_header={0};BITMAPINFOHEADER info={0};
 unsigned bytes=app_frame_width*app_frame_height*4;
 file_header.bfType=0x4d42;file_header.bfOffBits=sizeof(file_header)+sizeof(info);file_header.bfSize=file_header.bfOffBits+bytes;
 info.biSize=sizeof(info);info.biWidth=app_frame_width;info.biHeight=-(LONG)app_frame_height;
 info.biPlanes=1;info.biBitCount=32;info.biSizeImage=bytes;
 int ok=fwrite(&file_header,1,sizeof(file_header),file)==sizeof(file_header) && fwrite(&info,1,sizeof(info),file)==sizeof(info) && fwrite(app_frame_pixels,1,bytes,file)==bytes;
 if(fclose(file) || !ok) {fputs("Display snapshot write failed\n",stderr);ExitProcess(33);}
}
static int app_input_focus;
static int app_foreground_owned;
static int app_resume_notifications;
static int app_operation_notifications,app_performance_notifications;
/* Desktop compatibility profile; independent of host CPU/GPU clock controls. */
static unsigned app_operation_mode=1,app_performance_mode=PERFORMANCE_MODE;
static int app_focus_policy, app_suspend_background, app_suspend_out_of_focus, app_exit_requested;
static uint64_t app_suspended_ticks;
static unsigned app_focus=3, app_messages[64], app_message_head, app_message_count;
static void app_enqueue(unsigned message) {
 if(app_message_count==64) boundary("applet-message-overflow",&state,state.pc);
 app_messages[(app_message_head+app_message_count++)%64]=message;
 if(!SetEvent(app_message_event)) boundary("applet-event-signal",&state,state.pc);
}
static void app_set_focus(unsigned focus) {
 if(app_focus!=focus) {app_focus=focus;app_enqueue(15);printf("WINDOW focus=%u\n",focus);fflush(stdout);}
}
/* AC_HANDHELD=1: the game is told it runs handheld (operation mode 0, normal performance mode), as on a Switch out of
 * its dock: it then renders at its handheld resolution, 1280x720, instead of the docked 1600x900. */
static void app_modes_load(void) {
 static int done;char v[4];
 if(done) return;
 done=1;
 if(GetEnvironmentVariableA("AC_HANDHELD",v,sizeof v)==1 && v[0]=='1') {
  app_operation_mode=0;app_performance_mode=0;
  printf("[app] handheld mode (AC_HANDHELD=1)\n");fflush(stdout);
 }
}
static void app_set_modes(unsigned operation,unsigned performance) {
 if(operation>1 || performance>1) boundary("invalid-application-mode",&state,state.pc);
 if(app_operation_mode!=operation) {
  app_operation_mode=operation;if(app_operation_notifications) app_enqueue(30);
 }
 if(app_performance_mode!=performance) {
  app_performance_mode=performance;if(app_performance_notifications) app_enqueue(31);
 }
}
static int app_receive(unsigned *message) {
 if(!app_message_count) return 0;
 *message=app_messages[app_message_head];app_message_head=(app_message_head+1)%64;
 if(!--app_message_count && !ResetEvent(app_message_event)) boundary("applet-event-reset",&state,state.pc);
 return 1;
}
static volatile ULONGLONG app_save_write_tick;  /* (save_fs.h) */
static DWORD WINAPI app_exit_watchdog(LPVOID unused) {
 (void)unused;Sleep(1500);
 /* A save being written is not cut off (a write goes straight into the save file): wait until writes have been quiet
  * for 1 s, at most 5 s more. */
 for(int i=0;i<50 && GetTickCount64()-app_save_write_tick<1000;i++) Sleep(100);
 rumble_stop();
 fflush(stdout);fflush(stderr);TerminateProcess(GetCurrentProcess(),0);return 0;
}
/* Fullscreen: a borderless window covering its monitor (no display mode change; the swapchain follows the window
 * size and the picture keeps its aspect ratio, device.c). F11 or Alt+Enter toggles it (unless the key is a game key in
 * the launcher's Controls); AC_FULLSCREEN=1 starts in it (nvn_host_window). */
static int app_fullscreen;static WINDOWPLACEMENT app_windowed_place={sizeof(WINDOWPLACEMENT)};
static void app_set_fullscreen(HWND window,int on) {
 if(!window || on==app_fullscreen) return;
 if(on) {
  MONITORINFO info={sizeof info};
  if(!GetWindowPlacement(window,&app_windowed_place) || !GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&info)) return;
  SetWindowLongPtrW(window,GWL_STYLE,(GetWindowLongPtrW(window,GWL_STYLE)&~WS_OVERLAPPEDWINDOW)|WS_POPUP);
  SetWindowPos(window,HWND_TOP,info.rcMonitor.left,info.rcMonitor.top,info.rcMonitor.right-info.rcMonitor.left,
   info.rcMonitor.bottom-info.rcMonitor.top,SWP_FRAMECHANGED|SWP_NOOWNERZORDER);
 } else {
  SetWindowLongPtrW(window,GWL_STYLE,(GetWindowLongPtrW(window,GWL_STYLE)&~WS_POPUP)|WS_OVERLAPPEDWINDOW);
  SetWindowPlacement(window,&app_windowed_place);
  SetWindowPos(window,NULL,0,0,0,0,SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER);
 }
 app_fullscreen=on;
 printf("[app] %s\n",on ? "fullscreen" : "windowed");
}
/* In-game menu (live_settings.h, las/menu.cpp) */
static void menu_set_open(int open) {
 HWND window=app_window;
 if((ac_menu_open!=0)==(open!=0)) return;
 if(open) {  /* (the game lets go of everything: keys, buttons, the mouse) */
  memset(hid_keys,0,sizeof(hid_keys));memset(keyboard_keys,0,sizeof(keyboard_keys));
  mouse_clear();mouse_keys_clear();if(window && GetCapture()==window) ReleaseCapture();
  mouse_set_captured(window,0);mouse_alt_free=0;
 }
 InterlockedExchange(&ac_menu_open,open ? 1 : 0);
 /* The game is told it is out of focus, as under the Switch's HOME menu; its threads are not stopped
  * (app_checkpoint), so it keeps presenting and the menu keeps drawing. AC_MENU_PAUSE=0: not told. */
 static int pause=-1;
 if(pause<0) {char v[4];DWORD n=GetEnvironmentVariableA("AC_MENU_PAUSE",v,sizeof(v));pause=!(n>0 && n<sizeof(v) && v[0]=='0');}
 if(pause && app_window) app_set_focus(open ? 2 : IsIconic(app_window) ? 3 : 1);
 printf("[menu] %s\n",open ? "open" : "closed");fflush(stdout);
}
int ac_fullscreen_state(void) { return app_fullscreen; }
#define WM_AC_FULLSCREEN (WM_APP+41)
/* From the menu (render thread). Window changes go to the window thread as a message. */
void ac_setting_apply(const char *key,const char *value) {
 int on=value && !strcmp(value,"1");
 if(!strcmp(key,"prompts")) ac_prompt_style=ac_prompt_parse(value);
 else if(!strcmp(key,"rumble")) {rumble_set_percent(atoi(value));rumble_update(rumble_focus);}
 else if(!strcmp(key,"mouse_look")) {mouse_config();mouse_look_on=on;ac_mouse_look=on;}
 else if(!strcmp(key,"mouse_sens")) {mouse_config();double s=atof(value)/100.0;if(s>=0.05 && s<=20) mouse_sens=s;}
 else if(!strcmp(key,"mouse_invert")) {mouse_config();mouse_invert=on;}
 else if(!strcmp(key,"cutscene_fps_30")) ac_cutscene_30=on;
 else if(!strcmp(key,"ignore_focus")) ac_ignore_focus=on;
 else if(!strcmp(key,"upscaler")) ac_upscaler=value && !strcmp(value,"fsr1");
 else if(!strcmp(key,"fsr_sharpness")) {float s=(float)atof(value);if(s>=0 && s<=2) ac_fsr_sharpness=s;}
 else if(!strcmp(key,"uncapped")) ac_uncapped=on;
 else if(!strcmp(key,"port_credit")) ac_title_credit=on;
 else if(!strcmp(key,"fps_counter")) ac_fps_counter=on;
 else if(!strcmp(key,"title_own_twin")) ac_title_own_twin=on;
 else if(!strcmp(key,"swap_ab")) ac_swap_ab=on;
 else if(!strcmp(key,"fullscreen")) {if(app_window) PostMessageW(app_window,WM_AC_FULLSCREEN,on,0);}
 else return;
 printf("[menu] %s=%s\n",key,value ? value : "");fflush(stdout);
}
static LRESULT CALLBACK app_window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
 /* Dragging by the title bar is done here: Windows' own move loop runs inside this call, on the game's main thread
  * and under the kernel lock, and held the whole game (sound included) for as long as the button was down. */
 static int dragging;static POINT grab;
 if(message==WM_NCLBUTTONDOWN && wparam==HTCAPTION && !IsZoomed(window)) {
  POINT p;RECT r;GetCursorPos(&p);GetWindowRect(window,&r);
  grab.x=p.x-r.left;grab.y=p.y-r.top;dragging=1;SetCapture(window);return 0;
 }
 if(dragging) {
  if(message==WM_MOUSEMOVE) {
   POINT p;GetCursorPos(&p);
   SetWindowPos(window,NULL,p.x-grab.x,p.y-grab.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);return 0;
  }
  if(message==WM_LBUTTONUP) {dragging=0;ReleaseCapture();return 0;}
  if(message==WM_CAPTURECHANGED) {dragging=0;return 0;}
 }
 if(message==WM_PAINT && app_frame_pixels) {
  PAINTSTRUCT paint;HDC dc=BeginPaint(window,&paint);RECT client;GetClientRect(window,&client);
  app_paint_frame(dc,&client);EndPaint(window,&paint);return 0;
 }
 if(message==WM_AC_FULLSCREEN) {app_set_fullscreen(window,(int)wparam);return 0;}
 if(ac_menu_open) {  /* (the menu has the mouse and the keyboard) */
  if(message==WM_MOUSEMOVE) {menu_push(1,(int16_t)LOWORD(lparam),(int16_t)HIWORD(lparam));return 0;}
  if(message==WM_LBUTTONDOWN || message==WM_LBUTTONUP || message==WM_RBUTTONDOWN || message==WM_RBUTTONUP ||
     message==WM_MBUTTONDOWN || message==WM_MBUTTONUP) {
   int button=message<=WM_LBUTTONUP ? 0 : message<=WM_RBUTTONUP ? 1 : 2;
   int down=message==WM_LBUTTONDOWN || message==WM_RBUTTONDOWN || message==WM_MBUTTONDOWN;
   menu_push(1,(int16_t)LOWORD(lparam),(int16_t)HIWORD(lparam));menu_push(2,button,down);return 0;
  }
  if(message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL) {menu_push(3,(int16_t)HIWORD(wparam),message==WM_MOUSEHWHEEL);return 0;}
  if(message==WM_XBUTTONDOWN || message==WM_XBUTTONUP) return TRUE;
  if(message==WM_CHAR) {menu_push(5,(int)wparam,0);return 0;}
  if(message==WM_SETCURSOR && LOWORD(lparam)==HTCLIENT) {SetCursor(LoadCursorW(NULL,MAKEINTRESOURCEW(32512)));return TRUE;}
 }
 if(message==WM_MOUSEMOVE || message==WM_LBUTTONDOWN || message==WM_LBUTTONUP || message==WM_RBUTTONDOWN || message==WM_RBUTTONUP ||
    message==WM_MBUTTONDOWN || message==WM_MBUTTONUP || message==WM_XBUTTONDOWN || message==WM_XBUTTONUP || message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL) {
  if(app_input_focus && app_focus==1 && !app_exit_requested) hid_mouse_event(window,message,wparam,lparam);
  return message==WM_XBUTTONDOWN || message==WM_XBUTTONUP ? TRUE : 0;
 }
 if(message==WM_INPUT) {mouse_raw_input(lparam);return DefWindowProcW(window,message,wparam,lparam);}
 if(message==WM_SETCURSOR && mouse_captured && LOWORD(lparam)==HTCLIENT) {SetCursor(NULL);return TRUE;}
 if(message==WM_MOVE || message==WM_EXITSIZEMOVE) mouse_clip();
 /* (capture taken away with a button down: no game button stays held, and that button presses nothing until let go) */
 if(message==WM_CAPTURECHANGED) {if(mouse_buttons) {unsigned held=mouse_buttons;mouse_clear();mouse_keys_clear();mouse_swallow=held;}return 0;}
 if((message==WM_ACTIVATEAPP && !wparam) || (message==WM_SIZE && wparam==SIZE_MINIMIZED) || message==WM_CLOSE) {
  mouse_clear();if(window && GetCapture()==window) ReleaseCapture();
  mouse_set_captured(window,0);mouse_alt_free=0;
 }
 if(message==WM_ACTIVATEAPP) {
  app_input_focus=wparam!=0;
  if(!app_input_focus) {memset(hid_keys,0,sizeof(hid_keys));memset(keyboard_keys,0,sizeof(keyboard_keys));hid_publish(rt_ticks(),0);keyboard_publish(rt_ticks(),0);}
  return 0;
 }
 if((message==WM_KEYDOWN || message==WM_KEYUP || message==WM_SYSKEYDOWN || message==WM_SYSKEYUP) && wparam<256) {
  hid_keys_load();
  /* F1 opens and closes the in-game menu (unless F1 is a game key); Esc closes it. While it is open, keys go to it. */
  int down=message==WM_KEYDOWN || message==WM_SYSKEYDOWN,repeat=(lparam&(1<<30))!=0;
  if((wparam==VK_F1 && !hid_key_mapped[VK_F1]) || (ac_menu_open && !ac_menu_modal && wparam==VK_ESCAPE)) {
   if(down && !repeat) menu_set_open(wparam==VK_F1 ? !ac_menu_open : 0);
   return 0;
  }
  if(ac_menu_open && wparam!=VK_F11 && !(wparam==VK_RETURN && (lparam&(1<<29)))) {
   menu_push(4,(int)wparam,down);
   if(message==WM_SYSKEYDOWN || message==WM_SYSKEYUP) return DefWindowProcW(window,message,wparam,lparam);  /* (Alt+F4) */
   return 0;
  }
  int alt_enter=wparam==VK_RETURN && (lparam&(1<<29)) && !hid_key_mapped[VK_MENU];
  if((wparam==VK_F11 && !hid_key_mapped[VK_F11]) || alt_enter) {
   if((message==WM_KEYDOWN || message==WM_SYSKEYDOWN) && !(lparam&(1<<30))) app_set_fullscreen(window,!app_fullscreen);
   return 0;
  }
  if(app_input_focus && app_focus==1 && !app_exit_requested) {
   int down=message==WM_KEYDOWN || message==WM_SYSKEYDOWN;
   hid_keys[wparam]=down;keyboard_event((unsigned)wparam,lparam,down);
   /* (holding Alt gives the mouse back, unless Alt is a game key; letting go takes it again) */
   if(wparam==VK_MENU && !hid_key_mapped[VK_MENU]) {
    if(down && mouse_captured) {mouse_set_captured(window,0);mouse_alt_free=1;}
    else if(!down && mouse_alt_free) {mouse_alt_free=0;mouse_set_captured(window,1);mouse_swallow=mouse_buttons;}  /* (as a click) */
   }
  }
  /* (Alt and F10 open the window menu: not when they are game keys. Alt+F4 still closes. Alt alone frees the mouse
   * instead: letting go of it does not enter the window menu, whose loop would hold the game.) */
  hid_keys_load();
  if(message==WM_SYSKEYUP && wparam==VK_MENU) return 0;
  if((message==WM_SYSKEYDOWN || message==WM_SYSKEYUP) && !((wparam==VK_MENU || wparam==VK_F10) && hid_key_mapped[wparam]))
   return DefWindowProcW(window,message,wparam,lparam);
  return 0;
 }
 if(message==WM_SIZE) {
  mouse_clip();
  app_set_focus(wparam==SIZE_MINIMIZED ? 3 : ac_menu_open ? 2 : 1);
  if(app_focus==3) {memset(hid_keys,0,sizeof(hid_keys));memset(keyboard_keys,0,sizeof(keyboard_keys));hid_publish(rt_ticks(),0);keyboard_publish(rt_ticks(),0);}
  return 0;
 }
 if(message==WM_CLOSE) {
  rumble_stop();
  /* The game gets the exit request; if shutdown stalls (GPU work, waits), the
   * process must not linger invisibly holding the save lock and GPU memory. */
  if(!app_exit_requested) {HANDLE t=CreateThread(NULL,0,app_exit_watchdog,NULL,0,NULL);if(t)CloseHandle(t);}
  ShowWindow(window,SW_HIDE);  /* (closing looks immediate; the sound is muted too: wave_output.h) */
  app_exit_requested=1;app_enqueue(4);return 0;
 }
 return DefWindowProcW(window,message,wparam,lparam);
}
/* The host thread that created the window: only it can pump the window's messages (parallel mode). */
static DWORD app_window_thread;
static void app_pump(void) {
 if(!app_window || GetCurrentThreadId()!=app_window_thread) return;
 if(ac_menu_close_request) {InterlockedExchange(&ac_menu_close_request,0);menu_set_open(0);}
 if(ac_menu_quit_request) {InterlockedExchange(&ac_menu_quit_request,0);PostMessageW(app_window,WM_CLOSE,0,0);}
 MSG message;
 unsigned count=0;
 /* (stall reports) this runs under the kernel lock: a slow message or input read stops every guest thread. Steps over
  * 30 ms are logged ("PUMP"). */
 LARGE_INTEGER f,a,b;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&a);
 for(;;) {
  BOOL got=PeekMessageW(&message,NULL,0,0,PM_REMOVE);
  QueryPerformanceCounter(&b);
  double ms=1000.0*(double)(b.QuadPart-a.QuadPart)/(double)f.QuadPart;
  if(ms>30.0) printf("PUMP PeekMessage took %.0f ms\n",ms);
  if(!got) break;
  if(++count>4096) boundary("host-message-budget",&state,state.pc);
  {const char *w=rt_where("dispatch message");TranslateMessage(&message);DispatchMessageW(&message);rt_where(w);}
  QueryPerformanceCounter(&a);
  ms=1000.0*(double)(a.QuadPart-b.QuadPart)/(double)f.QuadPart;
  if(ms>30.0) printf("PUMP message 0x%x took %.0f ms\n",message.message,ms);
 }
 QueryPerformanceCounter(&a);
 /* (the active window without WM_ACTIVATEAPP having said so, e.g. activated before it was shown: input counts) */
 if(!app_input_focus && GetForegroundWindow()==app_window) {app_input_focus=1;printf("[app] input focus (foreground)\n");}
 {const char *w=rt_where("hid_poll");hid_poll(app_input_focus && (app_focus==1 || ac_menu_open) && !app_exit_requested);rt_where(w);}
 QueryPerformanceCounter(&b);
 double ms=1000.0*(double)(b.QuadPart-a.QuadPart)/(double)f.QuadPart;
 if(ms>30.0) printf("PUMP input read took %.0f ms\n",ms);
}
static void app_checkpoint(void) {
 clock_refresh();
 app_pump();
 if(!app_window || !app_focus_policy || app_exit_requested) return;
 /* AC_IGNORE_FOCUS=1: keep running while the window is unfocused or in the background (unattended runs). */
 if(ac_ignore_focus<0) {char v[4];DWORD n=GetEnvironmentVariableA("AC_IGNORE_FOCUS",v,sizeof(v));ac_ignore_focus=n>0 && n<sizeof(v) && v[0]!='0';}
 if(ac_ignore_focus) return;
 uint64_t start=0;
 while((app_focus==3 && app_suspend_background) || (app_focus==2 && app_suspend_out_of_focus && !ac_menu_open)) {
  if(!start) start=rt_ticks();
  const char *was=rt_where("suspended");
  unsigned held=klock_release_all();  /* (parallel mode: other threads' services must not wait on this) */
  DWORD waited=MsgWaitForMultipleObjectsEx(0,NULL,16,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
  klock_reacquire(held);rt_where(was);
  if(waited==WAIT_FAILED)
   boundary("host-suspension-wait",&state,state.pc);
  app_pump();
  if(app_exit_requested) break;
 }
 if(start) {
  app_suspended_ticks+=rt_ticks()-start;
  if(app_suspend_event && !SetEvent(app_suspend_event)) boundary("suspended-time-event",&state,state.pc);
  if(app_resume_notifications && !app_exit_requested) app_enqueue(16);
 }
}
static void app_open(void) {
 if(app_window) return;
 if(!app_message_event) {
  app_message_event=CreateEventW(NULL,TRUE,FALSE,NULL);
  if(!app_message_event) boundary("host-applet-event-create",&state,state.pc);
 }
 WNDCLASSW window_class={0};
 window_class.lpfnWndProc=app_window_proc;
 window_class.hInstance=GetModuleHandleW(NULL);
 window_class.lpszClassName=L"AstralChainNativeStartup";
 window_class.hCursor=LoadCursorW(NULL,MAKEINTRESOURCEW(32512));
 window_class.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
 if(!RegisterClassW(&window_class)) boundary("host-window-class",&state,state.pc);
 app_window=CreateWindowExW(0,window_class.lpszClassName,L"Astral Chain native startup - graphics not implemented",
  WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,960,540,NULL,NULL,window_class.hInstance,NULL);
 if(!app_window) boundary("host-window-create",&state,state.pc);
 app_window_thread=GetCurrentThreadId();
 /* The window is brought to the front: until it is the active window, the game reads no keys and no controllers
  * (WM_ACTIVATEAPP). Not with AC_IGNORE_FOCUS=1 (unattended runs, which must not take the desktop's focus). */
 {char v[4];DWORD n=GetEnvironmentVariableA("AC_IGNORE_FOCUS",v,sizeof(v));int quiet=n>0 && n<sizeof(v) && v[0]!='0';
  ShowWindow(app_window,quiet ? SW_SHOWNOACTIVATE : SW_SHOW);
  if(!quiet && !SetForegroundWindow(app_window)) {printf("[app] window not brought to the front\n");fflush(stdout);}}
 UpdateWindow(app_window);app_pump();
 app_set_focus(IsIconic(app_window) ? 3 : 1);
 /* Publish initial application state even if no size transition occurred. */
 if(!app_message_count) app_enqueue(15);
}
static DWORD app_wait(DWORD count,const HANDLE *handles,DWORD timeout) {
 if(!app_window) return WaitForMultipleObjects(count,handles,FALSE,timeout);
 ULONGLONG start=GetTickCount64();
 for(;;) {
  app_pump();
  DWORD result=WaitForMultipleObjects(count,handles,FALSE,0);
  if(result!=WAIT_TIMEOUT) return result;
  ULONGLONG elapsed=GetTickCount64()-start;
  if(timeout!=INFINITE && elapsed>=timeout) return WAIT_TIMEOUT;
  DWORD slice=timeout==INFINITE ? 16 : (DWORD)((timeout-elapsed)<16 ? timeout-elapsed : 16);
  if(MsgWaitForMultipleObjectsEx(0,NULL,slice,QS_ALLINPUT,MWMO_INPUTAVAILABLE)==WAIT_FAILED) return WAIT_FAILED;
 }
}
