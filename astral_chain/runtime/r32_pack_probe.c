/* Pack completed R32 readback using captured target-0/layer-0 layout.
 * Diagnostic staging only; output padding is 0xcd, never claimed as guest data. */
#include "gpu_r32_surface.h"
#include <stdio.h>
#include <stdlib.h>
static int read_exact(const char *path,void *bytes,size_t size) {
 FILE *f=fopen(path,"rb");if(!f)return 0;
 size_t n=fread(bytes,1,size,f);int extra=fgetc(f),bad=ferror(f);fclose(f);
 return n==size && extra==EOF && !bad;
}
int main(int argc,char **argv) {
 if(argc!=4){fprintf(stderr,"usage: r32_pack_probe graphics-state.bin linear-r32.bin packed.bin\n");return 2;}
 uint8_t snapshot[16+GPU_GRAPHICS_METHODS*5];GpuGraphicsState g={0};uint32_t header[4];
 if(!read_exact(argv[1],snapshot,sizeof(snapshot))){fprintf(stderr,"snapshot extent\n");return 2;}
 memcpy(header,snapshot,16);
 if(header[0]!=0x53474341 || header[1]!=1 || header[2]!=GPU_GRAPHICS_METHODS || header[3])return 2;
 memcpy(g.values,snapshot+16,sizeof(g.values));memcpy(g.valid,snapshot+16+sizeof(g.values),sizeof(g.valid));
 for(unsigned i=0;i<GPU_GRAPHICS_METHODS;i++)if(g.valid[i]>1)return 2;
 GpuR32Surface surface;const char *error=gpu_r32_description(&g,0,0,&surface);
 if(error){fprintf(stderr,"%s\n",error);return 2;}
 size_t size=(size_t)surface.width*surface.height*4;
 uint8_t *source=malloc(size),*destination=malloc((size_t)surface.extent);int status=2;
 if(!source || !destination)goto done;
 if(!read_exact(argv[2],source,size)){fprintf(stderr,"pixel extent\n");goto done;}
 memset(destination,0xcd,(size_t)surface.extent);
 error=gpu_r32_pack(&surface,destination,(size_t)surface.extent,source,size);
 if(error){fprintf(stderr,"%s\n",error);goto done;}
 FILE *f=fopen(argv[3],"wbx");if(!f){perror("exclusive output");goto done;}
 size_t written=fwrite(destination,1,(size_t)surface.extent,f);int closed=fclose(f);
 if(written!=surface.extent || closed){fprintf(stderr,"incomplete output\n");goto done;}
 printf("target=0 layer=0 address=0x%llx width=%u height=%u bytes=%llu padding=0xcd staging-only=1\n",
  (unsigned long long)surface.address,surface.width,surface.height,(unsigned long long)surface.extent);status=0;
done:
 free(source);free(destination);return status;
}
