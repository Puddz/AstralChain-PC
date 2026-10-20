/* Raw RomFS image from an extracted RomFS folder, with LASRecomp's MIT builder (runtime/romfs_build.c):
 *   romfs-build <folder> <image>
 * Used for the v1.0.1 image: the update's RomFS is a patch over the base NCA; NSTool extracts the merged tree. */
#include <stdio.h>
#include "romfs_build.h"
static void progress(uint64_t done,uint64_t total) {
 static uint64_t last;
 if(done-last>=(1ull<<30) || done==total) {last=done;fprintf(stderr,"%llu / %llu MB\n",(unsigned long long)(done>>20),(unsigned long long)(total>>20));}
}
int main(int argc,char **argv) {
 if(argc!=3) {fprintf(stderr,"usage: romfs-build folder image\n");return 2;}
 char why[256]={0};
 if(!romfs_build(argv[1],argv[2],progress,why,sizeof why)) {fprintf(stderr,"failed: %s\n",why);return 1;}
 return 0;
}
