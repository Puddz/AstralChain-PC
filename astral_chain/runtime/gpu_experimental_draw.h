/* Opt-in live adapter for the reviewed first quad only. Unknown reset state
 * and clip elimination remain experimental; other draw shapes reject. */
#pragma once
#include "vulkan_draw_api.h"
#include <wchar.h>
static struct {const wchar_t *name;unsigned size;const char *sha;uint8_t *bytes;} draw_files[]={
 {L"state.bin",17936,"1ff623adf7e65924b7138f9f56f379b1811fa9e87d384e32f4a50ef2d866edfc",NULL},
 {L"vertex-original.bin",4096,"35ea1ab7f925b258e89b7da4e4b9c2a581c5400770a69b0ae14d42b15b5fbc8a",NULL},
 {L"fragment-original.bin",4096,"c5ceffb6891f0bfb9a15d49608be294f010b8834e560267f5f189b8f76738398",NULL},
 {L"vertex0.bin",48,"b08397f21a9d3af1aa51e617e90c88c0078c89ef0344814fc964d08c4e10c62f",NULL},
 {L"vertex1.bin",32,"57a4ad6a5dda1d1cb845be6039169c69a740bc7ee39cd385ffa082da7104b769",NULL},
 {L"vertex.spv",5920,"45641f5482ca407fda5b7457faa0f4b6e26baa529bc28bf670e66a56ab291e24",NULL},
 {L"fragment.spv",6432,"71922e5157cc0be23b67843575b51771c83fc359cde0cf820dce0edd9642c999",NULL},
 {L"sampler.bin",32,"3ef0f73f91303550ea4b2618e3be6a0ddbd5634fbc2b1a120e703a410fba9463",NULL}
};
static AcVulkanDrawFunction experimental_draw_function;
static int experimental_draw_loaded;
static void experimental_draw_dump(const char *name,const void *bytes,DWORD size) {
 HANDLE file=CreateFileA(name,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
 if(file==INVALID_HANDLE_VALUE)return; /* Capture first success; never overwrite. */
 DWORD written=0;BOOL ok=WriteFile(file,bytes,size,&written,NULL),closed=CloseHandle(file);
 printf("GPU experimental output file=%s bytes=%lu complete=%u\n",name,written,ok && closed && written==size);
}
static int draw_hash_matches(uint8_t *bytes,unsigned size,const char *expected) {
 BCRYPT_ALG_HANDLE algorithm=NULL;uint8_t digest[32];
 if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,NULL,0))return 0;
 NTSTATUS status=BCryptHash(algorithm,NULL,0,bytes,size,digest,32);BCryptCloseAlgorithmProvider(algorithm,0);
 if(status)return 0;char hex[65];for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",digest[i]);
 return !strcmp(hex,expected);
}
static const char *experimental_draw_load(void) {
 if(experimental_draw_loaded)return experimental_draw_loaded==1 ? NULL : "experimental draw package previously rejected";
 experimental_draw_loaded=-1;
 wchar_t directory[32768],path[32768];DWORD count=GetEnvironmentVariableW(L"AC_EXPERIMENTAL_DRAW_PACKAGE",directory,32768);
 if(!count || count>=32700)return "experimental draw package path unavailable";
 for(unsigned i=0;i<8;i++) {
  if(swprintf(path,32768,L"%ls\\%ls",directory,draw_files[i].name)<0)return "experimental draw package path";
  FILE *file=_wfopen(path,L"rb");if(!file)return "experimental draw artifact unavailable";
  uint8_t *bytes=malloc(draw_files[i].size);if(!bytes){fclose(file);return "experimental draw artifact allocation";}
  size_t got=fread(bytes,1,draw_files[i].size,file);int extra=fgetc(file),bad=ferror(file);fclose(file);
  if(got!=draw_files[i].size || extra!=EOF || bad || !draw_hash_matches(bytes,draw_files[i].size,draw_files[i].sha)) {
   free(bytes);return "experimental draw artifact identity mismatch";
  }
  draw_files[i].bytes=bytes;
 }
 if(swprintf(path,32768,L"%ls\\draw.dll",directory)<0)return "experimental draw DLL path";
 HMODULE module=LoadLibraryExW(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
 if(!module)return "experimental draw DLL load failed";
 experimental_draw_function=(AcVulkanDrawFunction)GetProcAddress(module,"ac_vulkan_draw");
 if(!experimental_draw_function){FreeLibrary(module);return "experimental draw DLL export missing";}
 experimental_draw_loaded=1;printf("GPU experimental first-draw package loaded\n");return NULL;
}
static int experimental_draw_address_register(unsigned method) {
 switch(method*4) {
  case 0x800:case 0x804:case 0x155c:case 0x1560:case 0x1574:case 0x1578:
  case 0x1608:case 0x160c:case 0x17c8:case 0x17cc:
  case 0x1c04:case 0x1c08:case 0x1c14:case 0x1c18:
  case 0x1f00:case 0x1f04:case 0x1f08:case 0x1f0c:case 0x2044:case 0x2144:return 1;
  default:return method>=0x3400/4; /* MME scratch, not rendering configuration. */
 }
}
static const char *experimental_draw_state(const GpuEngineState *engine,uint32_t count) {
 if(count!=4 || !engine->draw.active || engine->draw.topology!=5 || engine->draw.instance_id || engine->draw.primitive_id)
  return "experimental draw primitive unsupported";
 const uint32_t *values=(const uint32_t*)(draw_files[0].bytes+16);
 const uint8_t *valid=draw_files[0].bytes+16+GPU_GRAPHICS_METHODS*4;
 for(unsigned i=0;i<GPU_GRAPHICS_METHODS;i++) {
  if(experimental_draw_address_register(i))continue;
  if(engine->graphics.valid[i]!=valid[i] || (valid[i] && engine->graphics.values[i]!=values[i])) {
   printf("GPU experimental state mismatch offset=0x%x expected-valid=%u actual-valid=%u expected=0x%x actual=0x%x\n",
    i*4,valid[i],engine->graphics.valid[i],values[i],engine->graphics.values[i]);
   return "experimental draw state differs from reviewed case";
  }
 }
 return NULL;
}
static const char *experimental_draw_cbuf(const GpuEngineState *e,unsigned stage,unsigned slot,void *bytes) {
 if(!e->constant_buffers[stage][slot].known || !e->constant_buffers[stage][slot].enabled || e->constant_buffers[stage][slot].size!=256)
  return "experimental draw uniform extent unsupported";
 return gpu_capture_read(e->memory_context,e->constant_buffers[stage][slot].address,256,bytes,0);
}
static GpuDrawResult gpu_experimental_draw(const GpuEngineState *engine,uint32_t count) {
 const char *error=experimental_draw_load();if(error)return (GpuDrawResult){error,0};
 error=experimental_draw_state(engine,count);if(error)return (GpuDrawResult){error,0};
 const GpuGraphicsState *g=&engine->graphics;GpuAddressSpace *space=engine->memory_context;
 uint8_t windows[2][4096],vertices[2][48],uniforms[3][256],indices[8];uint64_t address;
 for(unsigned i=0;i<2;i++) {
  error=gpu_shader_window(space,g,i?5:1,&address,windows[i]);if(error)return (GpuDrawResult){error,0};
  if(memcmp(windows[i],draw_files[i+1].bytes,4096))return (GpuDrawResult){"experimental draw shader identity mismatch",0};
  unsigned base=0x1c00/4+i*4,limit=0x1f00/4+i*2,size=i?32:48;
  if(!g->valid[base+1] || !g->valid[base+2] || !g->valid[limit] || !g->valid[limit+1])return (GpuDrawResult){"experimental vertex address unknown",0};
  address=((uint64_t)g->values[base+1]<<32)|g->values[base+2];
  uint64_t last=((uint64_t)g->values[limit]<<32)|g->values[limit+1];
  if(last<address || last-address!=size-1)return (GpuDrawResult){"experimental vertex extent mismatch",0};
  error=gpu_capture_read(space,address,size,vertices[i],0);if(error)return (GpuDrawResult){error,0};
  if(memcmp(vertices[i],draw_files[3+i].bytes,size))return (GpuDrawResult){"experimental quad geometry mismatch",0};
 }
 GpuIndexRange index;error=gpu_index_range(g,count,&index);if(error)return (GpuDrawResult){error,0};
 if(index.width!=2 || index.first || index.size!=8)return (GpuDrawResult){"experimental index format mismatch",0};
 error=gpu_capture_read(space,index.address,8,indices,0);if(error)return (GpuDrawResult){error,0};
 for(unsigned i=0;i<4;i++)if(indices[i*2]!=i || indices[i*2+1])return (GpuDrawResult){"experimental index order mismatch",0};
 error=experimental_draw_cbuf(engine,0,3,uniforms[0]);if(error)return (GpuDrawResult){error,0};
 error=experimental_draw_cbuf(engine,0,4,uniforms[1]);if(error)return (GpuDrawResult){error,0};
 error=experimental_draw_cbuf(engine,4,4,uniforms[2]);if(error)return (GpuDrawResult){error,0};
 unsigned slot=g->values[0x2608/4];
 if(slot>=32 || !engine->constant_buffers[4][slot].known || !engine->constant_buffers[4][slot].enabled || engine->constant_buffers[4][slot].size<36)
  return (GpuDrawResult){"experimental texture cbuf unavailable",0};
 uint64_t cb=engine->constant_buffers[4][slot].address;
 if(cb>=(1ull<<40)-32)return (GpuDrawResult){"experimental texture cbuf overflow",0};
 uint32_t handle,tic[8],tsc[8];error=gpu_capture_read(space,cb+32,4,&handle,0);if(error)return (GpuDrawResult){error,0};
 unsigned ids[]={handle&0xfffff,handle>>20},pools[]={0x1574/4,0x155c/4};
 for(unsigned i=0;i<2;i++) {
  unsigned base=pools[i];if(!g->valid[base] || !g->valid[base+1] || g->values[base]>255 || ids[i]>g->values[base+2])return (GpuDrawResult){"experimental descriptor pool invalid",0};
  uint64_t pool=((uint64_t)g->values[base]<<32)|g->values[base+1];
  error=gpu_capture_read(space,pool+(uint64_t)ids[i]*32,32,i?tsc:tic,0);if(error)return (GpuDrawResult){error,0};
 }
 if(memcmp(tsc,draw_files[7].bytes,32))return (GpuDrawResult){"experimental sampler mismatch",0};
 GpuTextureExtent texture;error=gpu_texture_extent(tic,&texture);if(error)return (GpuDrawResult){error,0};
 GpuR32Surface target;error=gpu_r32_description(g,0,0,&target);if(error)return (GpuDrawResult){error,0};
 if(texture.width!=1600 || texture.height!=900 || texture.block_height!=4 || target.width!=1600 || target.height!=900 || target.block_height!=4)
  return (GpuDrawResult){"experimental draw dimensions mismatch",0};
 GpuMemoryView view=gpu_resolve(space,target.address,target.extent);
 if(!view.bytes || (view.kind!=0xfe && view.kind!=0xdb) || view.storage!=GPU_STORAGE_CANONICAL)return (GpuDrawResult){"experimental target mapping unavailable",0};
 uint64_t cpu=(uint64_t)(uintptr_t)view.bytes;Region region=query(cpu);
 if(!(region.permission&2) || cpu<region.base || cpu-region.base>=region.size || target.extent>region.size-(cpu-region.base))return (GpuDrawResult){"experimental target not writable",0};
 uint8_t *tiled=malloc((size_t)texture.size),*linear=malloc(1600*900*4),*output=malloc(1600*900*4);unsigned unknown=0;
 if(!tiled || !linear || !output){error="experimental draw allocation";goto done;}
 error=gpu_capture_read(space,texture.address,texture.size,tiled,1);if(error)goto done;
 for(unsigned y=0;y<900;y++)for(unsigned x=0;x<1600;x++) {
  uint64_t xb=x*4,at=(y/128)*128*6400+(xb/64)*8192+((y%128)/8)*512;
  at+=((xb%64)/32)*256+((y%8)/2)*64+((xb%32)/16)*32+(y%2)*16+xb%16;
  uint32_t word;memcpy(&word,tiled+at,4);
  if((word>>8)!=0xffffff){error="experimental depth input is not reviewed all-one case";goto done;}
  memcpy(linear+((uint64_t)y*1600+x)*4,&word,4);
 }
 AcVulkanDraw request={0};request.version=1;request.struct_size=sizeof(request);request.mode=1;
 request.inputs[0]=(AcDrawInput){draw_files[5].bytes,draw_files[5].size};request.inputs[1]=(AcDrawInput){draw_files[6].bytes,draw_files[6].size};
 request.inputs[2]=(AcDrawInput){vertices[0],48};request.inputs[3]=(AcDrawInput){vertices[1],32};
 request.inputs[4]=(AcDrawInput){uniforms[0],256};request.inputs[5]=(AcDrawInput){uniforms[1],256};
 request.inputs[6]=(AcDrawInput){indices,8};request.inputs[7]=(AcDrawInput){uniforms[2],256};request.inputs[8]=(AcDrawInput){linear,1600*900*4};
 request.output=output;request.output_size=1600*900*4;
 int result=experimental_draw_function(&request);
 if(result){unknown=result==3;error="experimental Vulkan draw failed";goto done;}
 error=gpu_store_canonical_r32(space,&target,output,1600*900*4);
 if(!error) {
  printf("GPU experimental indexed draw completed target=0x%llx pixels=1440000 writeback=1\n",(unsigned long long)target.address);
  experimental_draw_dump("gpu-experimental-linear-r32.bin",output,1600*900*4);
  experimental_draw_dump("gpu-experimental-target-r32.bin",view.bytes,(DWORD)target.extent);
 }
done:
 free(tiled);free(linear);free(output);return (GpuDrawResult){error,unknown};
}
