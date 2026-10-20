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
static DWORD WINAPI app_exit_watchdog(LPVOID unused) {
 (void)unused;Sleep(3000);fflush(stdout);fflush(stderr);TerminateProcess(GetCurrentProcess(),0);return 0;
}
static LRESULT CALLBACK app_window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
 if(message==WM_PAINT && app_frame_pixels) {
  PAINTSTRUCT paint;HDC dc=BeginPaint(window,&paint);RECT client;GetClientRect(window,&client);
  app_paint_frame(dc,&client);EndPaint(window,&paint);return 0;
 }
 if(message==WM_MOUSEMOVE || message==WM_LBUTTONDOWN || message==WM_LBUTTONUP || message==WM_RBUTTONDOWN || message==WM_RBUTTONUP ||
    message==WM_MBUTTONDOWN || message==WM_MBUTTONUP || message==WM_XBUTTONDOWN || message==WM_XBUTTONUP || message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL) {
  if(app_input_focus && app_focus==1 && !app_exit_requested) hid_mouse_event(window,message,wparam,lparam);
  return message==WM_XBUTTONDOWN || message==WM_XBUTTONUP ? TRUE : 0;
 }
 if(message==WM_CAPTURECHANGED) {if(mouse_buttons) mouse_clear();return 0;}
 if((message==WM_ACTIVATEAPP && !wparam) || (message==WM_SIZE && wparam==SIZE_MINIMIZED) || message==WM_CLOSE) {
  mouse_clear();if(window && GetCapture()==window) ReleaseCapture();
 }
 if(message==WM_ACTIVATEAPP) {
  app_input_focus=wparam!=0;
  if(!app_input_focus) {memset(hid_keys,0,sizeof(hid_keys));memset(keyboard_keys,0,sizeof(keyboard_keys));hid_publish(rt_ticks(),0);keyboard_publish(rt_ticks(),0);}
  return 0;
 }
 if((message==WM_KEYDOWN || message==WM_KEYUP || message==WM_SYSKEYDOWN || message==WM_SYSKEYUP) && wparam<256) {
  if(app_input_focus && app_focus==1 && !app_exit_requested) {
   int down=message==WM_KEYDOWN || message==WM_SYSKEYDOWN;
   hid_keys[wparam]=down;keyboard_event((unsigned)wparam,lparam,down);
  }
  if(message==WM_SYSKEYDOWN || message==WM_SYSKEYUP) return DefWindowProcW(window,message,wparam,lparam);
  return 0;
 }
 if(message==WM_SIZE) {
  app_set_focus(wparam==SIZE_MINIMIZED ? 3 : 1);
  if(app_focus==3) {memset(hid_keys,0,sizeof(hid_keys));memset(keyboard_keys,0,sizeof(keyboard_keys));hid_publish(rt_ticks(),0);keyboard_publish(rt_ticks(),0);}
  return 0;
 }
 if(message==WM_CLOSE) {
  /* The game gets the exit request; if shutdown stalls (GPU work, waits), the
   * process must not linger invisibly holding the save lock and GPU memory. */
  if(!app_exit_requested) {HANDLE t=CreateThread(NULL,0,app_exit_watchdog,NULL,0,NULL);if(t)CloseHandle(t);}
  app_exit_requested=1;app_enqueue(4);return 0;
 }
 return DefWindowProcW(window,message,wparam,lparam);
}
/* The host thread that created the window: only it can pump the window's messages (parallel mode). */
static DWORD app_window_thread;
static void app_pump(void) {
 if(!app_window || GetCurrentThreadId()!=app_window_thread) return;
 MSG message;
 unsigned count=0;
 while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)) {
  if(++count>4096) boundary("host-message-budget",&state,state.pc);
  TranslateMessage(&message);DispatchMessageW(&message);
 }
 hid_poll(app_input_focus && app_focus==1 && !app_exit_requested);
}
static void app_checkpoint(void) {
 clock_refresh();
 app_pump();
 if(!app_window || !app_focus_policy || app_exit_requested) return;
 /* AC_IGNORE_FOCUS=1: keep running while the window is unfocused or in the background (unattended runs). */
 static int ignore_focus=-1;
 if(ignore_focus<0) ignore_focus=GetEnvironmentVariableA("AC_IGNORE_FOCUS",NULL,0)!=0;
 if(ignore_focus) return;
 uint64_t start=0;
 while((app_focus==3 && app_suspend_background) || (app_focus==2 && app_suspend_out_of_focus)) {
  if(!start) start=rt_ticks();
  unsigned held=klock_release_all();  /* (parallel mode: other threads' services must not wait on this) */
  DWORD waited=MsgWaitForMultipleObjectsEx(0,NULL,16,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
  klock_reacquire(held);
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
 ShowWindow(app_window,SW_SHOWNOACTIVATE);UpdateWindow(app_window);app_pump();
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
