/* Writable save filesystem on the native save store (save_store.h).
 * Paths: same validation and case-sensitive lookup as save_file.h; the final
 * component may be absent. Writes go directly to the store's data directory and
 * Commit flushes; there is no journal, unlike Horizon's save containers.
 * Result codes (switchbrew Filesystem services): PathNotFound 0x202,
 * PathAlreadyExists 0x402, DirectoryNotEmpty 0x1002 (desc 8). */
#pragma once
#include "save_file.h"
enum { SAVE_MISSING=0, SAVE_FILE=1, SAVE_DIRECTORY=2 };
#define SAVE_PATH_NOT_FOUND 0x202u
#define SAVE_PATH_EXISTS 0x402u
#define SAVE_DIRECTORY_NOT_EMPTY 0x1002u
/* Resolve a guest save path to a host path. kind: SAVE_MISSING/FILE/DIRECTORY.
 * Returns a Win32 error; ERROR_PATH_NOT_FOUND when a parent is missing. */
static DWORD save_resolve(SaveStore *store,const char *input,size_t extent,wchar_t path[1100],int *kind) {
 *kind=SAVE_MISSING;
 if(!store->ready)return ERROR_INVALID_STATE;
 if(!extent || extent>769)return ERROR_BAD_PATHNAME;
 const char *end=memchr(input,0,extent);
 if(!end || end==input || input[0]!='/')return ERROR_BAD_PATHNAME;
 swprintf(path,1100,L"\\\\?\\%ls\\data",store->root);
 if(end-input==1) {*kind=SAVE_DIRECTORY;return 0;}
 wchar_t relative[769];
 int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input+1,(int)(end-input-1),relative,768);
 if(!count)return ERROR_BAD_PATHNAME;
 relative[count]=0;
 if(relative[count-1]==L'/')relative[--count]=0; /* trailing separator on directories */
 size_t component=0;
 for(int i=0;i<=count;i++) {
  wchar_t c=relative[i];
  if(!c || c==L'/') {
   if(i==(int)component || relative[i-1]==L'.' || relative[i-1]==L' ' || i-component>255)return ERROR_BAD_PATHNAME;
   component=i+1;
  } else if(c<32 || wcschr(L"\\:*?\"<>|",c))return ERROR_BAD_PATHNAME;
 }
 size_t start=0;
 for(int i=0;i<=count;i++) if(!relative[i] || relative[i]==L'/') {
  wchar_t saved=relative[i];relative[i]=0;
  wcscat(path,L"\\");wcscat(path,relative+start);
  WIN32_FIND_DATAW found;HANDLE find=FindFirstFileW(path,&found);
  if(find==INVALID_HANDLE_VALUE) {
   DWORD error=GetLastError();
   if(error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND)return error;
   if(saved)return ERROR_PATH_NOT_FOUND;
   return 0; /* missing final component */
  }
  FindClose(find);
  /* Console lookups are case-sensitive: an NTFS case alias is not the entry. */
  if(wcscmp(found.cFileName,relative+start))return saved ? ERROR_PATH_NOT_FOUND : ERROR_FILE_EXISTS;
  if(found.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)return ERROR_INVALID_DATA;
  int directory=(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
  if(saved && !directory)return ERROR_PATH_NOT_FOUND;
  if(!saved)*kind=directory ? SAVE_DIRECTORY : SAVE_FILE;
  relative[i]=saved;start=i+1;
 }
 return 0;
}
static uint64_t save_used_bytes(const wchar_t *directory) {
 wchar_t pattern[1110];swprintf(pattern,1110,L"%ls\\*",directory);
 WIN32_FIND_DATAW data;HANDLE find=FindFirstFileW(pattern,&data);uint64_t total=0;
 if(find==INVALID_HANDLE_VALUE)return 0;
 do {
  if(!wcscmp(data.cFileName,L".") || !wcscmp(data.cFileName,L".."))continue;
  if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) {
   wchar_t child[1110];swprintf(child,1110,L"%ls\\%ls",directory,data.cFileName);total+=save_used_bytes(child);
  } else total+=((uint64_t)data.nFileSizeHigh<<32)|data.nFileSizeLow;
 } while(FindNextFileW(find,&data));
 FindClose(find);return total;
}
static DWORD save_delete_tree(const wchar_t *directory) {
 wchar_t pattern[1110];swprintf(pattern,1110,L"%ls\\*",directory);
 WIN32_FIND_DATAW data;HANDLE find=FindFirstFileW(pattern,&data);
 if(find!=INVALID_HANDLE_VALUE) {
  do {
   if(!wcscmp(data.cFileName,L".") || !wcscmp(data.cFileName,L".."))continue;
   wchar_t child[1110];swprintf(child,1110,L"%ls\\%ls",directory,data.cFileName);
   DWORD error=data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY ? save_delete_tree(child) : (DeleteFileW(child)?0:GetLastError());
   if(error) {FindClose(find);return error;}
  } while(FindNextFileW(find,&data));
  FindClose(find);
 }
 return RemoveDirectoryW(directory)?0:GetLastError();
}
static uint64_t save_capacity(const SaveStore *store) {uint64_t size;memcpy(&size,store->identity+40,8);return size;}
/* Each commit is also copied, in the background, to <save>-snapshots\<date-time>\data (the newest 30 are kept):
 * checkpoints to load again later, e.g. the one just before a cutscene (tests with AC_DATA_DIR), and a backup.
 * AC_SAVE_SNAPSHOTS=0 turns this off. */
static void snapshot_copy(const wchar_t *from,const wchar_t *to) {
 CreateDirectoryW(to,NULL);
 wchar_t pattern[1100];swprintf(pattern,1100,L"%ls\\*",from);
 WIN32_FIND_DATAW f;HANDLE h=FindFirstFileW(pattern,&f);if(h==INVALID_HANDLE_VALUE)return;
 do {
  if(!wcscmp(f.cFileName,L".") || !wcscmp(f.cFileName,L".."))continue;
  wchar_t a[1100],b[1100];swprintf(a,1100,L"%ls\\%ls",from,f.cFileName);swprintf(b,1100,L"%ls\\%ls",to,f.cFileName);
  if(f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) snapshot_copy(a,b); else CopyFileW(a,b,FALSE);
 } while(FindNextFileW(h,&f));
 FindClose(h);
}
static void snapshot_delete(const wchar_t *path) {
 wchar_t pattern[1100];swprintf(pattern,1100,L"%ls\\*",path);
 WIN32_FIND_DATAW f;HANDLE h=FindFirstFileW(pattern,&f);
 if(h!=INVALID_HANDLE_VALUE) {
  do {
   if(!wcscmp(f.cFileName,L".") || !wcscmp(f.cFileName,L".."))continue;
   wchar_t a[1100];swprintf(a,1100,L"%ls\\%ls",path,f.cFileName);
   if(f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) snapshot_delete(a); else DeleteFileW(a);
  } while(FindNextFileW(h,&f));
  FindClose(h);
 }
 RemoveDirectoryW(path);
}
static wchar_t snapshot_root[221];
static volatile LONG snapshot_busy;
static DWORD WINAPI snapshot_thread(void *argument) {
 (void)argument;
 Sleep(500);  /* (writes made right after the commit call) */
 wchar_t dir[1100],from[1100],to[1100];
 swprintf(dir,1100,L"\\\\?\\%ls-snapshots",snapshot_root);CreateDirectoryW(dir,NULL);
 SYSTEMTIME t;GetLocalTime(&t);
 swprintf(to,1100,L"%ls\\%04u%02u%02u-%02u%02u%02u",dir,t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
 CreateDirectoryW(to,NULL);wcscat(to,L"\\data");
 swprintf(from,1100,L"\\\\?\\%ls\\data",snapshot_root);
 snapshot_copy(from,to);
 /* keep the newest 30 (names sort by time) */
 wchar_t pattern[1100],names[64][32];unsigned n=0;swprintf(pattern,1100,L"%ls\\*",dir);
 WIN32_FIND_DATAW f;HANDLE h=FindFirstFileW(pattern,&f);
 if(h!=INVALID_HANDLE_VALUE) {
  do if((f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && f.cFileName[0]!=L'.' && wcslen(f.cFileName)<32 && n<64)
   wcscpy(names[n++],f.cFileName);
  while(FindNextFileW(h,&f));
  FindClose(h);
 }
 while(n>30) {
  unsigned oldest=0;for(unsigned j=1;j<n;j++) if(wcscmp(names[j],names[oldest])<0) oldest=j;
  wchar_t old[1100];swprintf(old,1100,L"%ls\\%ls",dir,names[oldest]);snapshot_delete(old);
  wcscpy(names[oldest],names[--n]);
 }
 printf("SAVE snapshot %ls\n",to);
 snapshot_busy=0;
 return 0;
}
static void save_snapshot(SaveStore *store) {
 static int on=-1;
 if(on<0) {char v[8];DWORD n=GetEnvironmentVariableA("AC_SAVE_SNAPSHOTS",v,sizeof(v));on=!(n && n<sizeof(v) && v[0]=='0');}
 if(!on || !store->ready)return;
 if(InterlockedCompareExchange(&snapshot_busy,1,0))return;  /* one copy at a time */
 wcscpy(snapshot_root,store->root);
 HANDLE thread=CreateThread(NULL,0,snapshot_thread,NULL,0,NULL);
 if(thread) CloseHandle(thread); else snapshot_busy=0;
}
/* Returns 1 when handled; result is the Horizon result for the reply. */
static int save_fs_command(SaveStore *store,const IpcRequest *request,const uint8_t *input,
                           uint32_t *result,uint64_t *value,int *has_value,HANDLE *opened) {
 *result=0;*has_value=0;*opened=NULL;
 unsigned command=request->command;
 if(command==10) { /* Commit: writes are already in place; make them durable. */
  app_save_write_tick=GetTickCount64();printf("SAVE commit\n");save_snapshot(store);return 1;
 }
 if(command==11 || command==12) {
  wchar_t root[1100];swprintf(root,1100,L"\\\\?\\%ls\\data",store->root);
  uint64_t capacity=save_capacity(store),used=save_used_bytes(root);
  *value=command==12 ? capacity : (used<capacity ? capacity-used : 0);*has_value=1;return 1;
 }
 if(command>=9) return 0; /* OpenDirectory and others: unimplemented */
 if(request->send_count<1 || !request->send_is_pointer)boundary("save-path-layout",&state,state.pc);
 wchar_t path[1100];int kind;
 DWORD error=save_resolve(store,(const char*)input,(size_t)request->send_size,path,&kind);
 if(error==ERROR_PATH_NOT_FOUND || error==ERROR_FILE_EXISTS) {*result=SAVE_PATH_NOT_FOUND;return 1;}
 if(error) {printf("SAVE path win32=%lu\n",error);boundary("save-path",&state,state.pc);}
 switch(command) {
 case 0: { /* CreateFile(u32 option, u64 size, path) */
  if(request->argument_size<16)boundary("save-create-layout",&state,state.pc);
  uint64_t size;memcpy(&size,request->arguments+8,8);
  if(kind!=SAVE_MISSING) {*result=SAVE_PATH_EXISTS;return 1;}
  if(size>save_capacity(store))boundary("save-create-exceeds-capacity",&state,state.pc);
  HANDLE file=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);
  if(file==INVALID_HANDLE_VALUE) {printf("SAVE create win32=%lu\n",GetLastError());boundary("save-create-host",&state,state.pc);}
  LARGE_INTEGER end;end.QuadPart=(LONGLONG)size;
  if(!SetFilePointerEx(file,end,NULL,FILE_BEGIN) || !SetEndOfFile(file)) boundary("save-create-size",&state,state.pc);
  CloseHandle(file);return 1;
 }
 case 1: /* DeleteFile */
  if(kind!=SAVE_FILE) {*result=SAVE_PATH_NOT_FOUND;return 1;}
  if(!DeleteFileW(path))boundary("save-delete-host",&state,state.pc);
  return 1;
 case 2: /* CreateDirectory */
  if(kind!=SAVE_MISSING) {*result=SAVE_PATH_EXISTS;return 1;}
  if(!CreateDirectoryW(path,NULL))boundary("save-mkdir-host",&state,state.pc);
  return 1;
 case 3: /* DeleteDirectory */
  if(kind!=SAVE_DIRECTORY) {*result=SAVE_PATH_NOT_FOUND;return 1;}
  if(save_empty_directory(path)) {*result=SAVE_DIRECTORY_NOT_EMPTY;return 1;}
  if(!RemoveDirectoryW(path))boundary("save-rmdir-host",&state,state.pc);
  return 1;
 case 4: /* DeleteDirectoryRecursively */
  if(kind!=SAVE_DIRECTORY) {*result=SAVE_PATH_NOT_FOUND;return 1;}
  if(save_delete_tree(path))boundary("save-rmtree-host",&state,state.pc);
  return 1;
 case 5: case 6: { /* RenameFile / RenameDirectory: second path in the second pointer buffer */
  if(request->send_count<2 || !request->send_is_pointer2)boundary("save-rename-layout",&state,state.pc);
  if(kind!=(command==5 ? SAVE_FILE : SAVE_DIRECTORY)) {*result=SAVE_PATH_NOT_FOUND;return 1;}
  Region r=query(request->send_address2);
  if(!(r.permission&1) || request->send_size2>r.size-(request->send_address2-r.base))boundary("save-rename-memory",&state,state.pc);
  wchar_t target[1100];int target_kind;
  error=save_resolve(store,(const char*)(uintptr_t)request->send_address2,(size_t)request->send_size2,target,&target_kind);
  if(error==ERROR_PATH_NOT_FOUND) {*result=SAVE_PATH_NOT_FOUND;return 1;}
  if(error==ERROR_FILE_EXISTS || target_kind!=SAVE_MISSING) {*result=SAVE_PATH_EXISTS;return 1;}
  if(error)boundary("save-rename-path",&state,state.pc);
  if(!MoveFileExW(path,target,MOVEFILE_WRITE_THROUGH))boundary("save-rename-host",&state,state.pc);
  return 1;
 }
 case 7: /* GetEntryType: 0 directory, 1 file */
  if(kind==SAVE_MISSING) {*result=SAVE_PATH_NOT_FOUND;return 1;}
  *value=kind==SAVE_DIRECTORY ? 0 : 1;*has_value=2;return 1;
 case 8: { /* OpenFile(u32 mode: 1 read, 2 write, 4 append) */
  uint32_t mode=ipc_u32(request->arguments);
  if(!mode || mode&~7u)boundary("save-open-mode",&state,state.pc);
  if(kind!=SAVE_FILE) {*result=SAVE_PATH_NOT_FOUND;return 1;}
  DWORD access=(mode&1 ? GENERIC_READ : 0)|(mode&6 ? GENERIC_WRITE : 0);
  HANDLE file=CreateFileW(path,access,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
  if(file==INVALID_HANDLE_VALUE) {printf("SAVE open win32=%lu\n",GetLastError());boundary("save-open-host",&state,state.pc);}
  *opened=file;return 1;
 }
 }
 return 0;
}
static DWORD save_file_write(HANDLE file,uint64_t offset,const void *data,uint32_t size) {
 if(offset>INT64_MAX)return ERROR_INVALID_PARAMETER;
 LARGE_INTEGER position;position.QuadPart=(LONGLONG)offset;
 if(!SetFilePointerEx(file,position,NULL,FILE_BEGIN))return GetLastError();
 guest_io_prepare((uint64_t)(uintptr_t)data,size);
 app_save_write_tick=GetTickCount64();  /* (the exit watchdog waits for save writes to finish) */
 DWORD written=0;if(size && (!WriteFile(file,data,size,&written,NULL) || written!=size))return GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
 return 0;
}
