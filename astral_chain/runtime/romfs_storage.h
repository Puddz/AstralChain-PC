/* Process-wide read-only backing handle. Windows sharing excludes writers
 * and deletion while the native process holds the image. */
static HANDLE romfs_file=INVALID_HANDLE_VALUE;
static uint64_t romfs_size;
static void romfs_open(void) {
 if(romfs_file!=INVALID_HANDLE_VALUE) return;
#ifdef ROMFS_CONFIGURED
 wchar_t moved[MAX_PATH];DWORD n=GetEnvironmentVariableW(L"AC_DATA_DIR",moved,MAX_PATH-16);
 romfs_file=CreateFileW(n && n<MAX_PATH-16 ? wcscat(moved,L"\\romfs.bin") : romfs_path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
 if(romfs_file==INVALID_HANDLE_VALUE) boundary("romfs-open-failed",&state,state.pc);
 LARGE_INTEGER size;uint8_t header[80];DWORD read=0;
 if(!GetFileSizeEx(romfs_file,&size) || size.QuadPart<80 || (uint64_t)size.QuadPart!=ROMFS_EXPECTED_SIZE ||
    !ReadFile(romfs_file,header,80,&read,NULL) || read!=80 || memcmp(header,romfs_expected_header,80))
  boundary("romfs-backing-identity-mismatch",&state,state.pc);
 romfs_size=(uint64_t)size.QuadPart;
 printf("ROMFS opened read-only size=%llu\n",(unsigned long long)romfs_size);
#else
 boundary("missing-romfs-image",&state,state.pc);
#endif
}
/* The title screen (the "Port by Puddz" credit, las/menu.cpp): its scene, the twin of the newest save standing on a
 * roof, is a Bink video the game streams from the RomFS while it shows (movie/ev%04x.bk2; the game picks the event
 * from the newest save: main+0x6F6280; ev0000, a turning cube, when there is no save). A read inside one of those files marks the title as showing (las/menu.cpp
 * draws the credit while the last one is under 2 s old). The file table is read once to find their extents. */
static uint8_t *romfs_dirs,*romfs_files;static uint64_t romfs_dirs_size,romfs_files_size,romfs_data_at;
static int romfs_load_tables(void) {
 if(romfs_files) return 1;
 uint64_t h[10];DWORD read=0;LARGE_INTEGER p;p.QuadPart=0;
 if(!SetFilePointerEx(romfs_file,p,NULL,FILE_BEGIN) || !ReadFile(romfs_file,h,80,&read,NULL) || read!=80) return 0;
 if(h[4]>64u<<20 || h[8]>64u<<20) return 0;
 romfs_dirs=malloc(h[4]);romfs_files=malloc(h[8]);
 if(!romfs_dirs || !romfs_files) return 0;
 p.QuadPart=(LONGLONG)h[3];
 if(!SetFilePointerEx(romfs_file,p,NULL,FILE_BEGIN) || !ReadFile(romfs_file,romfs_dirs,(DWORD)h[4],&read,NULL) || read!=h[4]) return 0;
 p.QuadPart=(LONGLONG)h[7];
 if(!SetFilePointerEx(romfs_file,p,NULL,FILE_BEGIN) || !ReadFile(romfs_file,romfs_files,(DWORD)h[8],&read,NULL) || read!=h[8]) return 0;
 romfs_dirs_size=h[4];romfs_files_size=h[8];romfs_data_at=h[9];
 return 1;
}
/* Does the directory at offset dir (in the directory table) have the path path[0..len)? */
static int romfs_dir_is(uint32_t dir,const char *path,size_t len) {
 if(!dir) return len==0;
 if(dir+24>romfs_dirs_size) return 0;
 uint32_t parent,name_len;memcpy(&parent,romfs_dirs+dir,4);memcpy(&name_len,romfs_dirs+dir+20,4);
 if(dir+24+name_len>romfs_dirs_size || name_len>len) return 0;
 if(memcmp(romfs_dirs+dir+24,path+len-name_len,name_len)) return 0;
 if(!parent) return name_len==len;
 return name_len<len && path[len-name_len-1]=='/' && romfs_dir_is(parent,path,len-name_len-1);
}
/* The extent of the file at path (no leading slash) in the image; 0 if it is not there. */
static int romfs_extent(const char *path,uint64_t *start,uint64_t *size) {
 const char *slash=strrchr(path,'/');const char *name=slash ? slash+1 : path;size_t dir_len=slash ? (size_t)(slash-path) : 0;
 for(uint64_t o=0;o+32<=romfs_files_size;) {
  uint32_t parent,name_len;memcpy(&parent,romfs_files+o,4);memcpy(&name_len,romfs_files+o+28,4);
  if(o+32+name_len>romfs_files_size) break;
  if(name_len==strlen(name) && !memcmp(romfs_files+o+32,name,name_len) && romfs_dir_is(parent,path,dir_len)) {
   uint64_t offset;memcpy(&offset,romfs_files+o+8,8);memcpy(size,romfs_files+o+16,8);*start=romfs_data_at+offset;return 1;
  }
  o+=(32+name_len+3)&~3ull;
 }
 return 0;
}
static uint64_t title_video_start[16],title_video_end[16];static int title_videos=-1;
volatile unsigned long long ac_title_video_tick;  /* GetTickCount64 of the last read of a title video */
static void title_videos_find(void) {
 static const unsigned events[]={0x0,0x1,0x2,0x3,0x4,0x11,0x12,0x21,0x22,0x5001,0x5002,0x5011,0x5012,0x5021,0x5022};
 title_videos=0;
 if(!romfs_load_tables()) return;  /* (always: the credit can be turned on in the in-game menu) */
 for(unsigned i=0;i<sizeof events/sizeof *events && title_videos<16;i++) {
  char path[32];snprintf(path,sizeof path,"movie/ev%04x.bk2",events[i]);uint64_t start,size;
  if(romfs_extent(path,&start,&size)) {title_video_start[title_videos]=start;title_video_end[title_videos++]=start+size;}
 }
 printf("ROMFS title videos: %d\n",title_videos);
}
uint64_t rt_ticks(void);
static void romfs_read(uint64_t offset,uint64_t length,uint64_t address) {
 romfs_open();
 if(offset>romfs_size || length>romfs_size-offset || length>16*1024*1024)
  boundary("romfs-read-extent",&state,state.pc);
 Region destination=query(address);
 if(length && (!(destination.permission&2) || length>destination.size-(address-destination.base)))
  boundary("romfs-read-destination",&state,state.pc);
 if(!length) return;
 uint64_t t0=rt_ticks();
 guest_io_prepare(address,length);
 LARGE_INTEGER position;position.QuadPart=(LONGLONG)offset;DWORD read=0;
 if(!SetFilePointerEx(romfs_file,position,NULL,FILE_BEGIN) || !ReadFile(romfs_file,(void*)(uintptr_t)address,(DWORD)length,&read,NULL) || read!=length)
  boundary("romfs-read-failed",&state,state.pc);
 if(title_videos<0) title_videos_find();
 for(int i=0;i<title_videos;i++) if(offset<title_video_end[i] && offset+length>title_video_start[i]) {ac_title_video_tick=GetTickCount64();break;}
 uint64_t us=(rt_ticks()-t0)*10/192;
 printf("ROMFS read offset=0x%llx size=0x%llx us=%llu thread=%u\n",(unsigned long long)offset,(unsigned long long)length,(unsigned long long)us,current_thread);
}
