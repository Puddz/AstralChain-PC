/* BufferQueue producer connection, default queries and bounded parcel framing.
 * Preallocated slot acquisition does not render or present its contents. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "graphic_buffer.h"
/* Retain fails without changing ownership; success sets a nonzero reference.
 * Release cannot fail for a reference owned by this producer. */
typedef const char *(*BqRetainBuffer)(void *,BqGraphicBuffer *);
typedef void (*BqReleaseBuffer)(void *,unsigned);
typedef struct {
 uint64_t timestamp;
 unsigned automatic,left,top,right,bottom,scaling,transform,sticky,async,interval,fence_count;
 struct {unsigned id,value;} fences[4];
} BqFrameInput;
/* Synchronous consumption must finish reading guest pixels before success.
 * Failure leaves producer ownership unchanged and must not publish a frame. */
typedef const char *(*BqConsumeBuffer)(void *,const BqGraphicBuffer *,const BqFrameInput *);
typedef struct {
 unsigned api,controlled,width,height,format;
 BqGraphicBuffer buffers[64];
 uint8_t dequeued[64],requested[64];
 BqRetainBuffer retain;BqReleaseBuffer release;void *context;
 BqConsumeBuffer consume;uint64_t frames;
} BqProducer;
static uint32_t bq_u32(const uint8_t *p) {
 return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void bq_put32(uint8_t *p,uint32_t v) {for(unsigned j=0;j<4;j++) p[j]=(uint8_t)(v>>(8*j));}
static int bq_has_free(const BqProducer *p) {
 if(p->api) for(unsigned i=0;i<64;i++) if(p->buffers[i].reference && !p->dequeued[i]) return 1;
 return 0;
}
/* Failure leaves producer and output unchanged; Binder status is in the parcel. */
static const char *bq_transaction(BqProducer *producer,unsigned code,unsigned flags,const uint8_t *input,size_t size,
 uint8_t *output,size_t capacity,unsigned *written) {
 if(flags || (code!=1 && code!=3 && code!=7 && code!=9 && code!=10 && code!=11 && code!=14)) return "unsupported buffer producer transaction";
 if(size<16) return "producer parcel header";
 uint32_t length=bq_u32(input),offset=bq_u32(input+4),objects=bq_u32(input+8),objects_offset=bq_u32(input+12);
 if(offset<16 || (offset&3) || offset>size || length>size-offset || objects || objects_offset>size)
  return "producer parcel extent or objects";
 const char token[]="android.gui.IGraphicBufferProducer";
 unsigned chars=sizeof(token)-1,token_size=8+((sizeof(token)*2+3)&~3u);
 unsigned arguments=code==7 ? 96 : code==3 ? 20 : code==10 ? 12 : 4;
 if(code==14 ? length<token_size+8 : length!=token_size+arguments) return "producer parcel payload size";
 const uint8_t *p=input+offset;
 if(bq_u32(p)!=0x100 || bq_u32(p+4)!=chars) return "producer interface token header";
 for(unsigned j=0;j<=chars;j++) if(p[8+j*2]!=(uint8_t)token[j] || p[9+j*2]) return "producer interface token";
 p+=token_size;
 if(code==7) {
  if(capacity<36) return "producer queue output capacity";
  if(bq_u32(p+4)!=84 || bq_u32(p+8)) return "producer queue flattened extent or handles";
  unsigned slot=bq_u32(p);const uint8_t *f=p+12;
  BqFrameInput frame={0};frame.timestamp=gb_u64(f);frame.automatic=bq_u32(f+8);
  frame.left=bq_u32(f+12);frame.top=bq_u32(f+16);frame.right=bq_u32(f+20);frame.bottom=bq_u32(f+24);
  frame.scaling=bq_u32(f+28);frame.transform=bq_u32(f+32);frame.sticky=bq_u32(f+36);
  frame.async=bq_u32(f+40);frame.interval=bq_u32(f+44);frame.fence_count=bq_u32(f+48);
  if(frame.automatic>1 || frame.async>1 || frame.fence_count>4 || frame.interval>4) return "producer queue flags or fence count";
  for(unsigned i=0;i<4;i++) {frame.fences[i].id=bq_u32(f+52+i*8);frame.fences[i].value=bq_u32(f+56+i*8);}
  int32_t status=0;
  if(!producer->api) status=-19;
  else if(slot>=64 || !producer->dequeued[slot] || !producer->requested[slot] || !producer->buffers[slot].reference) status=-22;
  else {
   const BqGraphicBuffer *buffer=&producer->buffers[slot];
   if(frame.left>=frame.right || frame.top>=frame.bottom || frame.right>buffer->width || frame.bottom>buffer->height) status=-22;
   else {
    if(!producer->consume) return "producer display consumer unavailable";
    if(producer->frames==UINT64_MAX) return "producer frame counter exhausted";
    const char *error=producer->consume(producer->context,buffer,&frame);if(error) return error;
    producer->dequeued[slot]=0;producer->frames++;
   }
  }
  uint8_t reply[36]={0};bq_put32(reply,20);bq_put32(reply+4,16);bq_put32(reply+12,36);
  bq_put32(reply+16,producer->width);bq_put32(reply+20,producer->height);
  /* Copy consumption is complete: identity transform hint, no pending frames,
   * and subsequent dequeue needs no outstanding consumer fence. */
  bq_put32(reply+32,(uint32_t)status);memcpy(output,reply,36);*written=36;return NULL;
 }
 if(code==3) {
  unsigned async=bq_u32(p),width=bq_u32(p+4),height=bq_u32(p+8),format=bq_u32(p+12),usage=bq_u32(p+16);
  if(async>1) return "producer dequeue async flag";
  if(capacity<72) return "producer dequeue output capacity";
  int32_t status=0;unsigned slot=64;
  if(!producer->api) status=-19;
  else if((!width)!=(!height)) status=-22;
  else {
   for(unsigned i=0;i<64;i++) if(producer->buffers[i].reference && !producer->dequeued[i]) {slot=i;break;}
   if(slot==64) {
    if(!async) return "producer blocking dequeue unsupported";
    status=-11;
   } else {
    if(!width) {width=producer->width;height=producer->height;}
    if(!format) format=producer->format;
    BqGraphicBuffer *buffer=&producer->buffers[slot];
    if(width!=buffer->width || height!=buffer->height || format!=buffer->format || (usage&buffer->usage)!=usage) status=-19;
   }
  }
  uint8_t reply[72]={0};bq_put32(reply,56);bq_put32(reply+4,16);bq_put32(reply+12,72);
  bq_put32(reply+16,status ? 0xffffffffu : slot);bq_put32(reply+20,1);bq_put32(reply+24,36);
  /* No active consumer fence: free slots are either unused or fully copied
   * by the synchronous consumer before being released. */
  for(unsigned i=0;i<4;i++) bq_put32(reply+36+i*8,0xffffffffu);
  bq_put32(reply+68,(uint32_t)status);
  if(!status) producer->dequeued[slot]=1;
  memcpy(output,reply,72);*written=72;return NULL;
 }
 if(code==1) {
  unsigned slot=bq_u32(p);
  int valid=producer->api && slot<64 && producer->dequeued[slot] && producer->buffers[slot].reference;
  unsigned total=valid ? 396 : 24;
  if(capacity<total) return "producer request-buffer output capacity";
  uint8_t reply[396]={0};bq_put32(reply,total-16);bq_put32(reply+4,16);bq_put32(reply+12,total);
  bq_put32(reply+16,valid);
  if(valid) {
   bq_put32(reply+20,364);memcpy(reply+28,producer->buffers[slot].wire,364);
   producer->requested[slot]=1;
  } else bq_put32(reply+20,(uint32_t)-22);
  memcpy(output,reply,total);*written=total;return NULL;
 }
 if(code==14) {
  unsigned slot=bq_u32(p),present=bq_u32(p+4),remaining=length-token_size-8;
  if(!producer->api || slot>=64 || present>1) return "producer registration state or slot";
  if(producer->dequeued[slot]) return "producer registration while dequeued";
  if(capacity<16 || !producer->retain || !producer->release) return "producer registration service or capacity";
  BqGraphicBuffer replacement={0};
  if(present) {
   if(remaining<8 || bq_u32(p+12) || bq_u32(p+8)!=remaining-8) return "graphic buffer flattened extent";
   const char *error=gb_decode(p+16,remaining-8,&replacement);if(error) return error;
   error=producer->retain(producer->context,&replacement);if(error) return error;
  } else if(remaining) return "null graphic buffer payload";
  if(producer->buffers[slot].reference) producer->release(producer->context,producer->buffers[slot].reference);
  producer->buffers[slot]=replacement;
  producer->requested[slot]=0;
  if(present) {producer->width=replacement.width;producer->height=replacement.height;producer->format=replacement.format;}
  uint8_t reply[16]={0};bq_put32(reply+4,16);bq_put32(reply+12,16);
  memcpy(output,reply,16);*written=16;return NULL;
 }
 unsigned listener=code==10 ? bq_u32(p) : 0,api=bq_u32(p+(code==10 ? 4 : 0));
 unsigned controlled=code==10 ? bq_u32(p+8) : 0;
 if(listener || controlled>1) return "unsupported producer listener or control flag";
 unsigned payload=code==10 ? 20 : code==9 ? 8 : 4,total=16+payload;
 if(capacity<total) return "producer parcel output capacity";
 BqProducer next=*producer;int32_t status=0;uint8_t reply[36]={0};
 if(code==9) {
  if(api>2) return "unsupported producer query";
  bq_put32(reply+16,api==0 ? producer->width : api==1 ? producer->height : producer->format);
 }
 else if(api<1 || api>4 || (code==10 ? producer->api!=0 : producer->api!=api)) status=-22;
 else if(code==10) {next.api=api;next.controlled=controlled;}
 else {
  for(unsigned i=0;i<64;i++) if(next.buffers[i].reference) {
   next.release(next.context,next.buffers[i].reference);memset(&next.buffers[i],0,sizeof(next.buffers[i]));
  }
  next.api=0;next.controlled=0;
  memset(next.dequeued,0,sizeof(next.dequeued));memset(next.requested,0,sizeof(next.requested));
 }
 bq_put32(reply,payload);bq_put32(reply+4,16);bq_put32(reply+12,total);
 if(code==10 && !status) {
  bq_put32(reply+16,producer->width);bq_put32(reply+20,producer->height);
  /* Identity transform and zero pending buffers: none exist yet. */
 }
 bq_put32(reply+16+payload-4,(uint32_t)status);
 memcpy(output,reply,total);*producer=next;*written=total;return NULL;
}
