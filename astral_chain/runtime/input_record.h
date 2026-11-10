/* Input recording and replay: what the game receives from the controller (buttons and both sticks, after keyboard,
 * mouse and controllers are combined), so a problem met in play can be played again on another PC.
 *
 * AC_INPUT_RECORD=1 starts a recording at the game's first controller sample (launch): a folder
 * <save folder>-recordings\<date>_<time> (next to userdata\saves\...\<user>) with input.txt and save\data, a copy of the
 * save as it was at launch. AC_INPUT_RECORD=<folder> records into that folder instead (it must not exist yet or be
 * empty; its parent must exist).
 *
 * input.txt: '#' lines are comments; then one line per change: seconds since the first sample, game frames since then
 * (svc #0xB1 calls, ac_game_steps), the button mask (hex, hid_service.h bits: 0 A ... 9 ZR, 16-23 stick directions),
 * left x y, right x y (-32767..32767, up positive). Lines "@ <seconds> <frames> cutscene <0|1>" mark where the game's
 * cutscene mode turned off or on (svc #0xB1, ac_in_cutscene).
 *
 * AC_INPUT_REPLAY=<recording folder or its input.txt> plays it back from the first sample on, in place of all input,
 * until its last line (then input is live again). A recording folder with save\data replays from that save: it is
 * copied to <folder>\replay-save (replaced on every replay; the recording's own copy is never written) and used as the
 * save folder. Each line is applied when the game reaches its frame count (replaying the highway route on the test PC,
 * screenshots were closer to the recording's than by seconds); AC_INPUT_REPLAY_CLOCK=seconds follows the seconds.
 * Loading takes a different number of frames on another PC, so the frame count is lined up again at each "@ cutscene"
 * mark: when the replay reaches a mark's frame first, it waits there (up to 30 s) for the cutscene to start or end;
 * when the cutscene starts or ends first, the replay moves on to the mark (a cutscene skipped 3 s in skips 3 s in). */
static const wchar_t *application_save_path(void);  /* (save_service.h) */

/* ---- recording */
static int rec_state=-1;  /* -1 not started, 0 off, 1 recording */
static FILE *rec_file;
static uint64_t rec_origin,rec_flushed;
static LONG rec_steps0;
static HidSample rec_last;
static unsigned rec_lines;
static int rec_cutscene=-1;
static void input_record_start(uint64_t now) {
 rec_state=0;
 wchar_t v[200];DWORD n=GetEnvironmentVariableW(L"AC_INPUT_RECORD",v,200);
 if(!n || n>=200 || !wcscmp(v,L"0")) return;
 wchar_t dir[260];
 if(!wcscmp(v,L"1")) {
  SYSTEMTIME st;GetLocalTime(&st);
  const wchar_t *save=application_save_path();
  if(wcslen(save)>200) {printf("[input] recording: save folder path too long\n");return;}
  swprintf(dir,260,L"%ls-recordings",save);CreateDirectoryW(dir,NULL);
  swprintf(dir,260,L"%ls-recordings\\%04u-%02u-%02u_%02u-%02u-%02u",save,st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,
           st.wSecond);
 } else wcscpy(dir,v);
 if(!CreateDirectoryW(dir,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS) {
  printf("[input] recording: folder %ls not created (error %lu)\n",dir,GetLastError());return;
 }
 /* the save as it is now (the game has not loaded it yet) */
 wchar_t from[300],to[300];
 swprintf(from,300,L"%ls\\data",application_save_path());
 swprintf(to,300,L"%ls\\save",dir);CreateDirectoryW(to,NULL);
 swprintf(to,300,L"%ls\\save\\data",dir);
 DWORD error=GetFileAttributesW(from)==INVALID_FILE_ATTRIBUTES ? ERROR_FILE_NOT_FOUND : snapshot_copy(from,to);
 wchar_t path[300];swprintf(path,300,L"%ls\\input.txt",dir);
 rec_file=_wfopen(path,L"w");
 if(!rec_file) {printf("[input] recording: %ls not created\n",path);return;}
 SYSTEMTIME st;GetLocalTime(&st);
 fprintf(rec_file,"# Astral Chain PC input recording 1, started %04u-%02u-%02u %02u:%02u:%02u\n",st.wYear,st.wMonth,st.wDay,
         st.wHour,st.wMinute,st.wSecond);
 fprintf(rec_file,"# save at launch: %s\n",error ? "not copied" : "save\\data");
 fprintf(rec_file,"# seconds frames buttons left-x left-y right-x right-y\n");
 rec_origin=now;rec_flushed=now;rec_steps0=ac_game_steps;rec_state=1;
 printf("[input] recording to %ls (save %s)\n",dir,error ? "not copied" : "copied");fflush(stdout);
}
static void input_record_sample(uint64_t now,const HidSample *s) {
 if(rec_state<0) input_record_start(now);
 if(rec_state!=1) return;
 if(rec_cutscene<0) rec_cutscene=ac_in_cutscene;
 else if(ac_in_cutscene!=rec_cutscene) {
  rec_cutscene=ac_in_cutscene;
  fprintf(rec_file,"@ %.3f %ld cutscene %d\n",(double)(now-rec_origin)/19200000.0,(long)(ac_game_steps-rec_steps0),rec_cutscene);
 }
 if(rec_lines && s->buttons==rec_last.buttons && s->lx==rec_last.lx && s->ly==rec_last.ly && s->rx==rec_last.rx &&
    s->ry==rec_last.ry) return;
 fprintf(rec_file,"%.3f %ld %llx %d %d %d %d\n",(double)(now-rec_origin)/19200000.0,(long)(ac_game_steps-rec_steps0),
         (unsigned long long)s->buttons,s->lx,s->ly,s->rx,s->ry);
 rec_last=*s;rec_lines++;
 if(now-rec_flushed>=19200000) {fflush(rec_file);rec_flushed=now;}  /* (at most a second lost if the game is killed) */
}

/* ---- replay */
typedef struct { double t; long frames; uint64_t buttons; int32_t lx,ly,rx,ry; } InputEvent;
static int rp_state=-1;  /* -1 not loaded, 0 off, 1 replaying */
static InputEvent *rp_events;
static unsigned rp_count,rp_next;
static int rp_frames_clock;
typedef struct { long frames; int cutscene; } InputMark;
static InputMark *rp_marks;
static unsigned rp_nmarks,rp_next_mark;
static long rp_shift;      /* recorded frame = replay frame + rp_shift (moved at each mark) */
static int rp_cutscene=-1;
static uint64_t rp_waiting;  /* since when the replay waits at a mark */
static uint64_t rp_origin;
static LONG rp_steps0;
static wchar_t rp_dir[260];
static void input_replay_paths(void) {  /* rp_dir: the recording folder, or "" */
 static int done;
 if(done) return;
 done=1;
 wchar_t v[240];DWORD n=GetEnvironmentVariableW(L"AC_INPUT_REPLAY",v,240);
 if(!n || n>=240) return;
 DWORD a=GetFileAttributesW(v);
 if(a==INVALID_FILE_ATTRIBUTES) {printf("[input] replay: %ls not found\n",v);return;}
 wcscpy(rp_dir,v);
 if(!(a&FILE_ATTRIBUTE_DIRECTORY)) {wchar_t *s=wcsrchr(rp_dir,L'\\');wchar_t *f=wcsrchr(rp_dir,L'/');if(f>s) s=f;if(s) *s=0;else wcscpy(rp_dir,L".");}
}
static void input_replay_load(void) {
 rp_state=0;
 input_replay_paths();
 if(!rp_dir[0]) return;
 wchar_t v[240],path[300];GetEnvironmentVariableW(L"AC_INPUT_REPLAY",v,240);
 if(GetFileAttributesW(v)&FILE_ATTRIBUTE_DIRECTORY) swprintf(path,300,L"%ls\\input.txt",v);else wcscpy(path,v);
 FILE *f=_wfopen(path,L"r");
 if(!f) {printf("[input] replay: %ls not readable\n",path);return;}
 char line[256];unsigned cap=0;
 while(fgets(line,sizeof(line),f)) {
  InputEvent e;unsigned long long b;
  {double mt;InputMark m;
   if(line[0]=='@' && sscanf(line,"@ %lf %ld cutscene %d",&mt,&m.frames,&m.cutscene)==3) {
    InputMark *g=realloc(rp_marks,(rp_nmarks+1)*sizeof(*g));if(g) {rp_marks=g;rp_marks[rp_nmarks++]=m;}
    continue;
   }}
  if(line[0]=='#' || sscanf(line,"%lf %ld %llx %d %d %d %d",&e.t,&e.frames,&b,&e.lx,&e.ly,&e.rx,&e.ry)!=7) continue;
  e.buttons=b;
  if(rp_count==cap) {cap=cap ? cap*2 : 4096;InputEvent *g=realloc(rp_events,cap*sizeof(*g));if(!g) break;rp_events=g;}
  rp_events[rp_count++]=e;
 }
 fclose(f);
 if(!rp_count) {printf("[input] replay: no input in %ls\n",path);return;}
 char c[16];DWORD n=GetEnvironmentVariableA("AC_INPUT_REPLAY_CLOCK",c,sizeof(c));
 rp_frames_clock=!(n && n<sizeof(c) && !strcmp(c,"seconds"));
 printf("[input] replaying %ls: %u changes over %.1f s (%ld frames), by %s, %u cutscene marks\n",path,rp_count,
        rp_events[rp_count-1].t,rp_events[rp_count-1].frames,rp_frames_clock ? "frames" : "seconds",rp_nmarks);fflush(stdout);
 rp_state=1;
}
/* Replaces the sample with the recording's; 0 once the recording has ended (or there is none). */
static int input_replay_sample(uint64_t now,HidSample *s) {
 if(rp_state<0) input_replay_load();
 if(rp_state!=1) return 0;
 if(!rp_origin) {rp_origin=now;rp_steps0=ac_game_steps;}
 double t=(double)(now-rp_origin)/19200000.0;long frames=(long)(ac_game_steps-rp_steps0);
 if(rp_frames_clock && rp_nmarks) {
  if(rp_cutscene<0) rp_cutscene=ac_in_cutscene;
  else if(ac_in_cutscene!=rp_cutscene) {  /* (a cutscene started or ended: line up with the next such mark) */
   rp_cutscene=ac_in_cutscene;
   unsigned k=rp_next_mark;
   while(k<rp_nmarks && k<rp_next_mark+4 && rp_marks[k].cutscene!=rp_cutscene) k++;
   if(k<rp_nmarks && rp_marks[k].cutscene==rp_cutscene) {
    long shift=rp_marks[k].frames-frames;
    printf("[input] replay: cutscene %s at frame %ld, recorded at frame %ld (%+ld)\n",rp_cutscene ? "on" : "off",frames,
           rp_marks[k].frames,shift-rp_shift);fflush(stdout);
    rp_shift=shift;rp_next_mark=k+1;rp_waiting=0;
   } else {printf("[input] replay: cutscene %s at frame %ld, not in the recording here\n",rp_cutscene ? "on" : "off",frames);fflush(stdout);}
  }
  long at=frames+rp_shift;
  if(rp_next_mark<rp_nmarks && at>=rp_marks[rp_next_mark].frames) {  /* (ahead of the game: wait at the mark) */
   if(!rp_waiting) {rp_waiting=now;printf("[input] replay: waiting for cutscene %s (recorded at frame %ld)\n",
                     rp_marks[rp_next_mark].cutscene ? "on" : "off",rp_marks[rp_next_mark].frames);fflush(stdout);}
   if(now-rp_waiting>30ull*19200000) {
    printf("[input] replay: cutscene %s did not come in 30 s, going on\n",rp_marks[rp_next_mark].cutscene ? "on" : "off");
    fflush(stdout);rp_next_mark++;rp_waiting=0;
   } else rp_shift=rp_marks[rp_next_mark].frames-1-frames;
  }
  frames+=rp_shift;
 }
 while(rp_next<rp_count && (rp_frames_clock ? rp_events[rp_next].frames<=frames : rp_events[rp_next].t<=t)) rp_next++;
 if(rp_next==rp_count) {
  /* (the last change has been held for its own line's length, at least a second, then input is live again) */
  const InputEvent *last=&rp_events[rp_count-1];
  if(rp_frames_clock ? frames-last->frames>60 : t-last->t>1.0) {
   printf("[input] replay ended at %.1f s (frame %ld)\n",t,frames);fflush(stdout);rp_state=0;return 0;
  }
 }
 if(!rp_next) return 1;  /* (before the first line: nothing pressed; recordings start at 0) */
 const InputEvent *e=&rp_events[rp_next-1];
 s->buttons=e->buttons;s->lx=e->lx;s->ly=e->ly;s->rx=e->rx;s->ry=e->ry;
 return 1;
}
/* The save folder for a replay of a recording that has one (see above), else NULL. */
static DWORD replay_clear(const wchar_t *dir) {  /* deletes the files and folders inside dir */
 wchar_t pattern[300];swprintf(pattern,300,L"%ls\\*",dir);
 WIN32_FIND_DATAW f;HANDLE h=FindFirstFileW(pattern,&f);
 if(h==INVALID_HANDLE_VALUE) return GetLastError()==ERROR_FILE_NOT_FOUND ? 0 : GetLastError();
 DWORD error=0;
 do {
  if(!wcscmp(f.cFileName,L".") || !wcscmp(f.cFileName,L"..")) continue;
  wchar_t p[300];swprintf(p,300,L"%ls\\%ls",dir,f.cFileName);
  if(f.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) {if(!(f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY ? RemoveDirectoryW(p) : DeleteFileW(p))) error=GetLastError();}
  else if(f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) {if(!(error=replay_clear(p)) && !RemoveDirectoryW(p)) error=GetLastError();}
  else if(!DeleteFileW(p)) error=GetLastError();
 } while(!error && FindNextFileW(h,&f));
 FindClose(h);
 return error;
}
static const wchar_t *input_replay_save_root(void) {
 static int done;static wchar_t root[221];
 if(done) return root[0] ? root : NULL;
 done=1;
 input_replay_paths();
 if(!rp_dir[0]) return NULL;
 wchar_t from[300],data[300];swprintf(from,300,L"%ls\\save\\data",rp_dir);
 if(GetFileAttributesW(from)==INVALID_FILE_ATTRIBUTES) {printf("[input] replay: the recording has no save; using yours\n");return NULL;}
 if(wcslen(rp_dir)>200) {printf("[input] replay: recording folder path too long for its save; using yours\n");return NULL;}
 swprintf(root,221,L"%ls\\replay-save",rp_dir);
 CreateDirectoryW(root,NULL);
 swprintf(data,300,L"%ls\\data",root);
 DWORD error=replay_clear(root);
 if(!error) error=snapshot_copy(from,data);
 /* (the manifest is this build's: the save store checks it, and a missing one empties the folder) */
 if(!error) {
  wchar_t m[300];swprintf(m,300,L"%ls\\manifest.bin",root);
  HANDLE h=CreateFileW(m,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);DWORD w=0;
  if(h==INVALID_HANDLE_VALUE) error=GetLastError();
  else {if(!WriteFile(h,application_save_identity,64,&w,NULL) || w!=64) error=GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;CloseHandle(h);}
 }
 if(error) {printf("[input] replay: save not prepared (error %lu); using yours\n",error);root[0]=0;return NULL;}
 printf("[input] replay: playing from the recording's save (%ls)\n",root);fflush(stdout);
 return root;
}
