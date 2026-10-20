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
static void romfs_read(uint64_t offset,uint64_t length,uint64_t address) {
 romfs_open();
 if(offset>romfs_size || length>romfs_size-offset || length>16*1024*1024)
  boundary("romfs-read-extent",&state,state.pc);
 Region destination=query(address);
 if(length && (!(destination.permission&2) || length>destination.size-(address-destination.base)))
  boundary("romfs-read-destination",&state,state.pc);
 if(!length) return;
 guest_io_prepare(address,length);
 LARGE_INTEGER position;position.QuadPart=(LONGLONG)offset;DWORD read=0;
 if(!SetFilePointerEx(romfs_file,position,NULL,FILE_BEGIN) || !ReadFile(romfs_file,(void*)(uintptr_t)address,(DWORD)length,&read,NULL) || read!=length)
  boundary("romfs-read-failed",&state,state.pc);
 printf("ROMFS read offset=0x%llx size=0x%llx\n",(unsigned long long)offset,(unsigned long long)length);
}
