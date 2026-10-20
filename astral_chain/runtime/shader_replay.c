/* Offline replay of runtime shader translations (research adapter for the reviewed Maxwell translator).
 * Inputs are the files LARECOMP_SAVE_MSL writes next to each translated shader: <addr>_<stage>.bin (the program
 * window exactly as passed to maxwell_translate) and <addr>_<stage>.key (the ShaderKey bytes). Output is the
 * generated GLSL, or the translator's error on stderr. Success means source generation only; the driver compiles
 * the GLSL separately. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shader.h"
int main(int argc,char **argv) {
 if(argc!=5) {fprintf(stderr,"usage: shader_replay window.bin key.bin vs|fs|cs output.glsl\n");return 2;}
 int stage;
 if(!strcmp(argv[3],"vs"))stage=STAGE_VERTEX;
 else if(!strcmp(argv[3],"fs"))stage=STAGE_FRAGMENT;
 else if(!strcmp(argv[3],"cs"))stage=STAGE_COMPUTE;
 else {fprintf(stderr,"unsupported stage\n");return 2;}
 static unsigned char blob[0x10000];
 FILE *input=fopen(argv[1],"rb");if(!input) {perror("window");return 2;}
 size_t size=fread(blob,1,sizeof blob,input);int bad=ferror(input);fclose(input);
 if(bad || !size) {fprintf(stderr,"empty or unreadable window\n");return 2;}
 ShaderKey key;
 input=fopen(argv[2],"rb");if(!input) {perror("key");return 2;}
 size_t got=fread(&key,1,sizeof key,input);int extra=fgetc(input);fclose(input);
 if(got!=sizeof key || extra!=EOF) {fprintf(stderr,"key size mismatch (expected %zu bytes)\n",sizeof key);return 2;}
 maxwell_gmem_base=0;maxwell_gmem_size=0x100000000ull;
 ShaderInfo *info=maxwell_translate(blob,size,stage,&key);
 if(!info) {fprintf(stderr,"translator allocation failed\n");return 1;}
 if(info->error[0] || !info->msl) {fprintf(stderr,"translation failed: %s\n",info->error);maxwell_free(info);return 1;}
 FILE *output=fopen(argv[4],"wb");if(!output) {perror("output");maxwell_free(info);return 2;}
 size_t n=strlen(info->msl);int failed=fwrite(info->msl,1,n,output)!=n;
 failed|=fclose(output)!=0;maxwell_free(info);
 return failed ? 2 : 0;
}
