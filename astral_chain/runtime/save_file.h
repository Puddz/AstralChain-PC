/* Bounded UTF-8 save paths and real read-only file handles. */
#pragma once
#include "save_store.h"
static DWORD save_file_open(SaveStore *store,const char *input,size_t extent,HANDLE *result) {
 *result=NULL;
 if(!store->ready)return ERROR_INVALID_STATE;
 if(!extent || extent>769)return ERROR_BAD_PATHNAME;
 const char *end=memchr(input,0,extent);
 if(!end || end==input || input[0]!='/' || end-input==1)return ERROR_BAD_PATHNAME;
 wchar_t relative[769];
 int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input+1,(int)(end-input-1),relative,768);
 if(!count)return ERROR_BAD_PATHNAME;
 relative[count]=0;size_t component=0;
 for(int i=0;i<=count;i++) {
  wchar_t c=relative[i];
  if(!c || c==L'/') {
   if(i==component || relative[i-1]==L'.' || relative[i-1]==L' ' || i-component>255)return ERROR_BAD_PATHNAME;
   component=i+1;
  } else if(c<32 || wcschr(L"\\:*?\"<>|",c))return ERROR_BAD_PATHNAME;
 }
 wchar_t path[1100];swprintf(path,1100,L"\\\\?\\%ls\\data",store->root);
 SaveStore held={0};DWORD error=0;size_t start=0;
 for(int i=0;i<=count;i++) if(!relative[i] || relative[i]==L'/') {
  wchar_t saved=relative[i];relative[i]=0;
  wcscat(path,L"\\");wcscat(path,relative+start);
  /* Preserve console case-sensitive lookup instead of silently opening an NTFS alias. */
  WIN32_FIND_DATAW found;HANDLE find=FindFirstFileW(path,&found);
  if(find==INVALID_HANDLE_VALUE) {error=GetLastError();goto done;}
  FindClose(find);
  if(wcscmp(found.cFileName,relative+start)) {error=ERROR_FILE_NOT_FOUND;goto done;}
  if(found.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) {error=ERROR_INVALID_DATA;goto done;}
  if(saved) {
   error=save_hold_directory(&held,path,0);if(error)goto done;
  } else {
   HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
   if(file==INVALID_HANDLE_VALUE) {error=GetLastError();goto done;}
   error=save_regular(file);
   if(error)CloseHandle(file);else *result=file;
  }
  relative[i]=saved;start=i+1;
 }
done:
 save_store_close(&held);return error;
}
static DWORD save_file_size(HANDLE file,uint64_t *size) {
 LARGE_INTEGER value;if(!GetFileSizeEx(file,&value))return GetLastError();
 if(value.QuadPart<0)return ERROR_INVALID_DATA;
 *size=(uint64_t)value.QuadPart;return 0;
}
static DWORD save_file_read(HANDLE file,uint64_t offset,void *output,uint32_t size,uint32_t *read) {
 *read=0;if(offset>INT64_MAX || size>16*1024*1024)return ERROR_INVALID_PARAMETER;
 LARGE_INTEGER position;position.QuadPart=(LONGLONG)offset;
 if(!SetFilePointerEx(file,position,NULL,FILE_BEGIN))return GetLastError();
 guest_io_prepare((uint64_t)(uintptr_t)output,size);
 DWORD actual=0;if(size && !ReadFile(file,output,size,&actual,NULL))return GetLastError();
 *read=actual;return 0;
}
