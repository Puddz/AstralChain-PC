/* Native PC save-store provisioning. This is not a Horizon save-container
 * implementation. Identity is durable; file/commit IPC is implemented separately.
 * Directory handles exclude deletion/renaming while the store is open. */
#pragma once
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef struct {
 wchar_t root[221];uint8_t identity[64];
 HANDLE directories[64],lock,manifest;
 unsigned directory_count,ready;
} SaveStore;

static void save_store_close(SaveStore *store) {
 if(store->manifest && store->manifest!=INVALID_HANDLE_VALUE)CloseHandle(store->manifest);
 if(store->lock && store->lock!=INVALID_HANDLE_VALUE)CloseHandle(store->lock);
 while(store->directory_count)CloseHandle(store->directories[--store->directory_count]);
 memset(store,0,sizeof(*store));
}
static DWORD save_regular(HANDLE file) {
 BY_HANDLE_FILE_INFORMATION info;
 if(!GetFileInformationByHandle(file,&info))return GetLastError();
 return info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)?ERROR_INVALID_DATA:0;
}
static DWORD save_hold_directory(SaveStore *store,const wchar_t *path,unsigned create) {
 if(store->directory_count==64)return ERROR_TOO_MANY_OPEN_FILES;
 if(create && !CreateDirectoryW(path,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS)return GetLastError();
 HANDLE directory=CreateFileW(path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,
  OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
 if(directory==INVALID_HANDLE_VALUE)return GetLastError();
 BY_HANDLE_FILE_INFORMATION info;DWORD error=0;
 if(!GetFileInformationByHandle(directory,&info))error=GetLastError();
 else if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)
  error=ERROR_INVALID_DATA;
 if(error)CloseHandle(directory);else store->directories[store->directory_count++]=directory;
 return error;
}
static DWORD save_empty_directory(const wchar_t *path) {
 wchar_t pattern[260];swprintf(pattern,260,L"%ls\\*",path);
 WIN32_FIND_DATAW data;HANDLE find=FindFirstFileW(pattern,&data);
 if(find==INVALID_HANDLE_VALUE) {DWORD error=GetLastError();return error==ERROR_FILE_NOT_FOUND?0:error;}
 DWORD error=0;
 do {
  if(wcscmp(data.cFileName,L".") && wcscmp(data.cFileName,L"..")) {error=ERROR_DIR_NOT_EMPTY;break;}
 } while(FindNextFileW(find,&data));
 if(!error && GetLastError()!=ERROR_NO_MORE_FILES)error=GetLastError();
 FindClose(find);return error;
}
static DWORD save_manifest_open(SaveStore *store,const wchar_t *path,const uint8_t identity[64]) {
 store->manifest=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
 if(store->manifest==INVALID_HANDLE_VALUE) {store->manifest=NULL;return GetLastError();}
 DWORD error=save_regular(store->manifest);if(error)return error;
 LARGE_INTEGER size;uint8_t bytes[64];DWORD read=0;
 if(!GetFileSizeEx(store->manifest,&size))return GetLastError();
 if(size.QuadPart!=64)return ERROR_INVALID_DATA;
 if(!ReadFile(store->manifest,bytes,64,&read,NULL))return GetLastError();
 return read!=64 || memcmp(bytes,identity,64)?ERROR_INVALID_DATA:0;
}
static DWORD save_store_ensure(SaveStore *store,const wchar_t *root,const uint8_t identity[64]) {
 wchar_t normalized[221];size_t length=wcslen(root);uint64_t program,data_size,journal_size;
 memcpy(&program,identity+8,8);memcpy(&data_size,identity+40,8);memcpy(&journal_size,identity+48,8);
 const uint8_t zero[16]={0};
 if(memcmp(identity,"ACPCSAV1",8) || !program || !memcmp(identity+24,zero,16) || !data_size ||
    data_size>INT64_MAX || journal_size>INT64_MAX-data_size || memcmp(identity+56,zero,8))return ERROR_INVALID_DATA;
 if(length<4 || length>220 || !((root[0]>=L'A' && root[0]<=L'Z') || (root[0]>=L'a' && root[0]<=L'z')) ||
    root[1]!=L':' || (root[2]!=L'\\' && root[2]!=L'/'))return ERROR_BAD_PATHNAME;
 wcscpy(normalized,root);
 size_t component=3;
 for(size_t i=2;i<=length;i++) {
  if(normalized[i]==L'/')normalized[i]=L'\\';
  if(i<3)continue;
  if(!normalized[i] || normalized[i]==L'\\') {
   if(i==component || normalized[i-1]==L'.' || normalized[i-1]==L' ')return ERROR_BAD_PATHNAME;
   component=i+1;
  } else if(normalized[i]<32 || wcschr(L":*?\"<>|",normalized[i]))return ERROR_BAD_PATHNAME;
 }
 if(store->ready)return wcscmp(store->root,normalized) || memcmp(store->identity,identity,64)?ERROR_INVALID_DATA:0;
 if(store->directory_count || store->lock || store->manifest)return ERROR_INVALID_STATE;
 DWORD error=0;wchar_t path[260],manifest[260],temporary[260];temporary[0]=0;unsigned own_temporary=0;
 wcscpy(store->root,normalized);
 /* Hold each ancestor before traversing into its child; do not follow junctions. */
 for(size_t i=3;i<=length;i++) if(normalized[i]==L'\\' || !normalized[i]) {
  wchar_t saved=normalized[i];normalized[i]=0;
  error=save_hold_directory(store,normalized,1);normalized[i]=saved;if(error)goto failed;
 }
 swprintf(path,260,L"%ls\\.lock",store->root);
 store->lock=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
 if(store->lock==INVALID_HANDLE_VALUE) {store->lock=NULL;error=GetLastError();goto failed;}
 if((error=save_regular(store->lock)))goto failed;
 LARGE_INTEGER lock_size;
 if(!GetFileSizeEx(store->lock,&lock_size)) {error=GetLastError();goto failed;}
 if(lock_size.QuadPart) {error=ERROR_INVALID_DATA;goto failed;}
 swprintf(manifest,260,L"%ls\\manifest.bin",store->root);
 error=save_manifest_open(store,manifest,identity);
 if(!error) {
  swprintf(path,260,L"%ls\\data",store->root);
  if((error=save_hold_directory(store,path,0)))goto failed;
 } else {
  if(error!=ERROR_FILE_NOT_FOUND)goto failed;
  swprintf(path,260,L"%ls\\data",store->root);
  if((error=save_hold_directory(store,path,1)))goto failed;
  if((error=save_empty_directory(path)))goto failed;
  ULARGE_INTEGER available;
  if(!GetDiskFreeSpaceExW(store->root,&available,NULL,NULL)) {error=GetLastError();goto failed;}
  if(available.QuadPart<data_size+journal_size) {error=ERROR_DISK_FULL;goto failed;}
  swprintf(temporary,260,L"%ls\\.new-%08lx-%016llx",store->root,GetCurrentProcessId(),(unsigned long long)GetTickCount64());
  HANDLE output=CreateFileW(temporary,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_FLAG_WRITE_THROUGH,NULL);
  if(output==INVALID_HANDLE_VALUE) {error=GetLastError();goto failed;}
  own_temporary=1;DWORD written=0;
  if(!WriteFile(output,identity,64,&written,NULL))error=GetLastError();
  else if(written!=64)error=ERROR_WRITE_FAULT;
  else if(!FlushFileBuffers(output))error=GetLastError();
  else error=0;
  CloseHandle(output);if(error)goto failed;
  if(!MoveFileExW(temporary,manifest,MOVEFILE_WRITE_THROUGH)) {error=GetLastError();goto failed;}
  own_temporary=0;
  if((error=save_manifest_open(store,manifest,identity)))goto failed;
 }
 memcpy(store->identity,identity,64);store->ready=1;return 0;
failed:
 if(own_temporary)DeleteFileW(temporary);
 save_store_close(store);return error;
}
