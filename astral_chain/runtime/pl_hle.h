/* nn::pl shared system fonts, replaced at the SDK exports (AC_HLE_SYMBOLS).
 * The console's system fonts are not part of the game. Preference, after
 * LASRecomp runtime/hle/pl.c (MIT): AC_FONT / AC_FONT_EXT (native_trace sets
 * them from a user dump in userdata/fonts); Windows Yu Gothic (closest style to the console's UD Shin Go); Arial.
 * SharedFontData (from the SDK's GetSharedFont): {u32 type; u32 pad;
 * const void *address; u64 size}. Types: 0 Standard, 1 ChineseSimple,
 * 2 ChineseSimpleExt, 3 ChineseTraditional, 4 Korean, 5 NintendoExt. */
#pragma once
typedef struct {uint8_t *data;uint64_t size;} PlFont;
static PlFont pl_fonts[2]; /* [0] standard (all text types), [1] NintendoExt if supplied */
static uint32_t pl_be32(const uint8_t *p) {return (uint32_t)p[0]<<24|p[1]<<16|p[2]<<8|p[3];}
static void pl_put32(uint8_t *p,uint32_t v) {p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
/* .bfttf: 8-byte header then u32 words XORed with a fixed key (either byte order). */
static size_t pl_bfttf_decode(uint8_t *d,size_t len) {
 static const uint32_t key=0x49621806;
 if(len<12) return 0;
 for(int swap=0;swap<2;swap++) {
  uint32_t k=swap ? __builtin_bswap32(key) : key,w1,w2;memcpy(&w1,d+4,4);memcpy(&w2,d+8,4);
  uint32_t size=w1^k,version=w2^k;
  if(!size || size>len-8 || (version!=0x00000100u && version!=0x4f54544fu)) continue;
  for(size_t i=0;i+4<=size;i+=4) {uint32_t w;memcpy(&w,d+8+i,4);w^=k;memcpy(d+i,&w,4);}
  return size;
 }
 return 0;
}
/* .ttc: face 0 as a standalone sfnt (the game's font engine wants one font). */
static uint8_t *pl_ttc_face0(const uint8_t *f,size_t len,size_t *out_len) {
 if(len<16 || memcmp(f,"ttcf",4)) return NULL;
 uint32_t o=pl_be32(f+12);if((uint64_t)o+12>len) return NULL;
 uint32_t tables=(uint32_t)f[o+4]<<8|f[o+5];
 if((uint64_t)o+12+16ull*tables>len) return NULL;
 size_t total=12+16*(size_t)tables;
 for(uint32_t i=0;i<tables;i++) total+=(pl_be32(f+o+12+16*i+12)+3)&~3u;
 uint8_t *out=calloc(1,total);if(!out) return NULL;
 memcpy(out,f+o,12);size_t pos=12+16*(size_t)tables;
 for(uint32_t i=0;i<tables;i++) {
  const uint8_t *e=f+o+12+16*i;uint32_t offset=pl_be32(e+8),n=pl_be32(e+12);
  if((uint64_t)offset+n>len) {free(out);return NULL;}
  memcpy(out+12+16*i,e,8);pl_put32(out+12+16*i+8,(uint32_t)pos);pl_put32(out+12+16*i+12,n);
  memcpy(out+pos,f+offset,n);pos+=(n+3)&~3u;
 }
 *out_len=total;return out;
}
static int pl_load(const char *path,PlFont *font) {
 FILE *f=fopen(path,"rb");if(!f) return 0;
 fseek(f,0,SEEK_END);long length=ftell(f);fseek(f,0,SEEK_SET);
 if(length<=0 || length>64*1024*1024) {fclose(f);return 0;}
 size_t len=(size_t)length;uint8_t *d=malloc(len+4);
 if(!d || fread(d,1,len,f)!=len) {fclose(f);free(d);return 0;}
 fclose(f);
 size_t n=strlen(path);
 if(n>6 && !_stricmp(path+n-6,".bfttf")) {
  len=pl_bfttf_decode(d,len);
  if(!len) {printf("PL %s: not a valid .bfttf\n",path);free(d);return 0;}
 } else if(len>=4 && !memcmp(d,"ttcf",4)) {
  size_t face_len;uint8_t *face=pl_ttc_face0(d,len,&face_len);free(d);
  if(!face) {printf("PL %s: bad font collection\n",path);return 0;}
  d=face;len=face_len;
 }
 font->data=d;font->size=len;printf("PL system font %s (%zu bytes)\n",path,len);return 1;
}
static void pl_load_fonts(void) {
 static int loaded;if(loaded) return;loaded=1;
 char custom[MAX_PATH];DWORD n=GetEnvironmentVariableA("AC_FONT",custom,sizeof(custom));
 const char *standard[]={n && n<sizeof(custom) ? custom : NULL,
  "C:/Windows/Fonts/YuGothR.ttc","C:/Windows/Fonts/YuGothM.ttc","C:/Windows/Fonts/arial.ttf"};
 for(size_t i=0;i<sizeof(standard)/sizeof(standard[0]) && !pl_fonts[0].data;i++) if(standard[i]) pl_load(standard[i],&pl_fonts[0]);
 if(!pl_fonts[0].data) boundary("pl-no-system-font",&state,state.pc);
 n=GetEnvironmentVariableA("AC_FONT_EXT",custom,sizeof(custom));
 if(n && n<sizeof(custom)) pl_load(custom,&pl_fonts[1]);
}
static const PlFont *pl_font_for(uint64_t type) {
 pl_load_fonts();return type==5 && pl_fonts[1].data ? &pl_fonts[1] : &pl_fonts[0];
}
/* bool nn::pl::GetSharedFont(int *count, SharedFontData *out, int max, LanguageCode) */
static void pl_get_shared_font(Cpu *c) {
 pl_load_fonts();
 int32_t max=(int32_t)c->x[2];uint64_t out=c->x[1];
 ST32(c->x[0],6);
 for(int32_t i=0;i<6 && i<max;i++) {
  const PlFont *font=pl_font_for((uint64_t)i);
  ST32(out+24*(uint64_t)i,(uint32_t)i);ST32(out+24*(uint64_t)i+4,0);
  ST64(out+24*(uint64_t)i+8,(uint64_t)(uintptr_t)font->data);ST64(out+24*(uint64_t)i+16,font->size);
 }
 c->x[0]=1;
}
static void pl_get_shared_font_address(Cpu *c) {c->x[0]=(uint64_t)(uintptr_t)pl_font_for(c->x[0])->data;}
static void pl_get_shared_font_size(Cpu *c) {c->x[0]=pl_font_for(c->x[0])->size;}
