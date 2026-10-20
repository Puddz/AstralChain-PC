/* Offline research adapter for the pinned LASRecomp Maxwell translator.
 * Input is exactly one 4096-byte GPU program window beginning at its SPH.
 * The translator implementation expects a preceding 0x30-byte NVN prefix.
 * Success means source generation only, not shader validation or execution. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "shader.h"
int main(int argc,char **argv) {
 if(argc!=5) {fprintf(stderr,"usage: shader_probe window.bin output.glsl vertex|fragment vertex-output-mask-hex\n");return 2;}
 int stage;
 if(!strcmp(argv[3],"vertex"))stage=STAGE_VERTEX;
 else if(!strcmp(argv[3],"fragment"))stage=STAGE_FRAGMENT;
 else {fprintf(stderr,"unsupported stage\n");return 2;}
 char *end;errno=0;unsigned long mask=strtoul(argv[4],&end,16);
 if(errno || argv[4][0]=='-' || end==argv[4] || *end || mask>0xfffffffful) {fprintf(stderr,"invalid varying mask\n");return 2;}
 unsigned char blob[0x30+4096]={0};
 FILE *input=fopen(argv[1],"rb");if(!input) {perror("input");return 2;}
 size_t got=fread(blob+0x30,1,4096,input);int extra=fgetc(input),bad=ferror(input);fclose(input);
 if(got!=4096 || extra!=EOF || bad) {fprintf(stderr,"expected exactly one 4096-byte shader window\n");return 2;}
 ShaderKey key={0};key.glsl=1;key.vs_out_mask=(uint32_t)mask;
 /* Float attributes/targets and default clip conventions are probe assumptions.
  * The real draw adapter must derive these fields from effective GPU state. */
 ShaderInfo *info=maxwell_translate(blob,sizeof(blob),stage,&key);
 if(!info) {fprintf(stderr,"translator allocation failed\n");return 1;}
 fprintf(stderr,"stage=%d instructions=%u attributes=0x%x outputs=0x%x cbufs=0x%x textures=%d depth=%u gmem=%u\n",
  stage,info->num_insts,info->attr_mask,info->out_mask,info->cbuf_mask,info->ntex,info->writes_depth,info->uses_gmem);
 if(info->error[0] || !info->msl) {fprintf(stderr,"translation failed: %s\n",info->error);maxwell_free(info);return 1;}
 for(unsigned i=0;i<18;i++)if(info->cbuf_mask&(1u<<i))fprintf(stderr,"cbuf=%u size=%u dynamic=%u\n",i,info->cbuf_size[i],(info->cbuf_dynamic>>i)&1);
 for(int i=0;i<info->ntex;i++)fprintf(stderr,"texture=%d handle_offset=0x%x type=%u shadow=%u bindless=%u\n",
  i,info->tex[i].handle_offset,info->tex[i].type,info->tex[i].shadow,info->tex[i].bindless);
 FILE *output=fopen(argv[2],"wbx");if(!output) {perror("output (must not exist)");maxwell_free(info);return 2;}
 size_t size=strlen(info->msl);int failed=fwrite(info->msl,1,size,output)!=size;
 failed|=fclose(output)!=0;maxwell_free(info);
 if(failed) {fprintf(stderr,"output incomplete\n");return 2;}
 return 0;
}
