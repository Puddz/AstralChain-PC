/* Windows PCM transport. No guest memory, IPC, synthetic playback clock or
 * fallback-success path. Serialize control calls and keep WaveOutput at a stable
 * address until close ends. A failed close must be retried before reuse. */
#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static struct {
 HMODULE module;
 __typeof__(&waveOutOpen) open;
 __typeof__(&waveOutPrepareHeader) prepare;
 __typeof__(&waveOutUnprepareHeader) unprepare;
 __typeof__(&waveOutWrite) write;
 __typeof__(&waveOutPause) pause;
 __typeof__(&waveOutRestart) restart;
 __typeof__(&waveOutReset) reset;
 __typeof__(&waveOutGetPosition) position;
 __typeof__(&waveOutClose) close;
} wave_api;

static MMRESULT wave_api_load(void) {
 if(wave_api.module)return MMSYSERR_NOERROR;
 HMODULE module=LoadLibraryExW(L"winmm.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
 if(!module)return MMSYSERR_NODRIVER;
#define WAVE_BIND(field,name) wave_api.field=(__typeof__(&name))GetProcAddress(module,#name);if(!wave_api.field)goto failed
 WAVE_BIND(open,waveOutOpen);WAVE_BIND(prepare,waveOutPrepareHeader);
 WAVE_BIND(unprepare,waveOutUnprepareHeader);WAVE_BIND(write,waveOutWrite);
 WAVE_BIND(pause,waveOutPause);WAVE_BIND(restart,waveOutRestart);
 WAVE_BIND(reset,waveOutReset);WAVE_BIND(position,waveOutGetPosition);WAVE_BIND(close,waveOutClose);
#undef WAVE_BIND
 wave_api.module=module;return MMSYSERR_NOERROR;
failed:
 FreeLibrary(module);memset(&wave_api,0,sizeof(wave_api));return MMSYSERR_NOTSUPPORTED;
}

typedef struct { WAVEHDR header; uint64_t tag; } WaveBuffer;
typedef struct {
 HWAVEOUT handle;
 HANDLE completion,closed;
 WAVEFORMATEX format;
 WaveBuffer buffers[32];
 unsigned head,count,running,ready;
 uint64_t queued_bytes,position_epoch;
 uint32_t last_position;
} WaveOutput;

static void CALLBACK wave_output_callback(HWAVEOUT wave,UINT message,DWORD_PTR context,DWORD_PTR first,DWORD_PTR second) {
 (void)wave;(void)first;(void)second;
 WaveOutput *output=(WaveOutput*)context;
 /* The callback must not call waveform APIs or touch guest/queue memory. */
 if(message==WOM_DONE)SetEvent(output->completion);
 if(message==WOM_CLOSE)SetEvent(output->closed);
}

static MMRESULT wave_output_open(WaveOutput *output,unsigned rate,unsigned channels) {
 if(output->handle || output->closed)return MMSYSERR_ALLOCATED;
 HANDLE completion=output->completion;  /* kept across a reopen: the guest holds copies of it */
 if(rate<8000 || rate>192000 || !channels || channels>8)return WAVERR_BADFORMAT;
 MMRESULT result=wave_api_load();if(result)return result;
 memset(output,0,sizeof(*output));output->completion=completion;
 output->format=(WAVEFORMATEX){.wFormatTag=WAVE_FORMAT_PCM,.nChannels=(WORD)channels,
  .nSamplesPerSec=rate,.nAvgBytesPerSec=rate*channels*2,.nBlockAlign=(WORD)(channels*2),.wBitsPerSample=16};
 if(!output->completion)output->completion=CreateEventW(NULL,TRUE,FALSE,NULL);
 output->closed=CreateEventW(NULL,TRUE,FALSE,NULL);
 if(!output->completion || !output->closed)result=MMSYSERR_NOMEM;
 else result=wave_api.open(&output->handle,WAVE_MAPPER,&output->format,
  (DWORD_PTR)wave_output_callback,(DWORD_PTR)output,CALLBACK_FUNCTION);
 if(result) {
  if(output->completion)CloseHandle(output->completion);
  if(output->closed)CloseHandle(output->closed);
  memset(output,0,sizeof(*output));return result;
 }
 /* No buffers are accepted until the caller handles a pause failure by closing. */
 result=wave_api.pause(output->handle);output->ready=!result;return result;
}

static MMRESULT wave_output_start(WaveOutput *output) {
 if(!output->handle || !output->ready)return MMSYSERR_INVALHANDLE;
 MMRESULT result=wave_api.restart(output->handle);
 if(!result)output->running=1;
 return result;
}
static MMRESULT wave_output_pause(WaveOutput *output) {
 if(!output->handle || !output->ready)return MMSYSERR_INVALHANDLE;
 MMRESULT result=wave_api.pause(output->handle);
 if(!result)output->running=0;
 return result;
}
static MMRESULT wave_output_append(WaveOutput *output,uint64_t tag,const void *pcm,size_t size) {
 if(!output->handle || !output->ready)return MMSYSERR_INVALHANDLE;
 if(!pcm || !size || size>16u*1024*1024 || size%output->format.nBlockAlign)return MMSYSERR_INVALPARAM;
 if(output->count==32 || output->queued_bytes+size>64u*1024*1024)return MMSYSERR_NOMEM;
 WaveBuffer *buffer=&output->buffers[(output->head+output->count)%32];
 if(buffer->header.lpData)return MMSYSERR_ALLOCATED;
 buffer->header.lpData=malloc(size);if(!buffer->header.lpData)return MMSYSERR_NOMEM;
 memcpy(buffer->header.lpData,pcm,size);buffer->header.dwBufferLength=(DWORD)size;buffer->tag=tag;
 {  /* AC_AUDIO_DUMP=<file>: every appended buffer as raw PCM (verification without listening) */
  static FILE *dump;static int tried;
  if(!tried) {
   tried=1;char path[MAX_PATH];DWORD n=GetEnvironmentVariableA("AC_AUDIO_DUMP",path,sizeof(path));
   if(n && n<sizeof(path)) {dump=fopen(path,"wb");
    printf("AUDIO dump %s rate=%lu channels=%u bits=16\n",path,(unsigned long)output->format.nSamplesPerSec,output->format.nChannels);}
  }
  if(dump) {fwrite(pcm,1,size,dump);fflush(dump);}
 }
 MMRESULT result=wave_api.prepare(output->handle,&buffer->header,sizeof(WAVEHDR));
 if(!result)result=wave_api.write(output->handle,&buffer->header,sizeof(WAVEHDR));
 if(result) {
  if(buffer->header.dwFlags&WHDR_PREPARED) {
   MMRESULT cleanup=wave_api.unprepare(output->handle,&buffer->header,sizeof(WAVEHDR));
   if(cleanup) {output->count++;output->queued_bytes+=size;output->ready=0;return cleanup;}
  }
  free(buffer->header.lpData);memset(buffer,0,sizeof(*buffer));return result;
 }
 output->count++;output->queued_bytes+=size;return MMSYSERR_NOERROR;
}
static MMRESULT wave_output_collect(WaveOutput *output,uint64_t *tags,unsigned capacity,unsigned *count) {
 if(!output->handle || !output->ready)return MMSYSERR_INVALHANDLE;
 if(!count || (capacity && !tags))return MMSYSERR_INVALPARAM;
 *count=0;
 while(*count<capacity && output->count) {
  WaveBuffer *buffer=&output->buffers[output->head];
  if(!(((volatile WAVEHDR*)&buffer->header)->dwFlags&WHDR_DONE))break;
  MMRESULT result=wave_api.unprepare(output->handle,&buffer->header,sizeof(WAVEHDR));
  if(result)return result;
  tags[(*count)++]=buffer->tag;output->queued_bytes-=buffer->header.dwBufferLength;
  free(buffer->header.lpData);memset(buffer,0,sizeof(*buffer));
  output->head=(output->head+1)%32;output->count--;
 }
 /* Clear before rechecking to avoid losing a concurrent WOM_DONE signal. */
 ResetEvent(output->completion);
 if(output->count && (((volatile WAVEHDR*)&output->buffers[output->head].header)->dwFlags&WHDR_DONE))
  SetEvent(output->completion);
 return MMSYSERR_NOERROR;
}
static MMRESULT wave_output_position(WaveOutput *output,uint64_t *frames) {
 if(!output->handle || !output->ready)return MMSYSERR_INVALHANDLE;
 if(!frames)return MMSYSERR_INVALPARAM;
 MMTIME position={.wType=TIME_SAMPLES};
 MMRESULT result=wave_api.position(output->handle,&position,sizeof(position));
 if(result)return result;
 if(position.wType!=TIME_SAMPLES)return MMSYSERR_NOTSUPPORTED;
 if(position.u.sample<output->last_position)output->position_epoch+=1ull<<32;
 output->last_position=position.u.sample;*frames=output->position_epoch+position.u.sample;
 return MMSYSERR_NOERROR;
}
/* A device Windows invalidated (waveOutWrite fails with MMSYSERR_INVALHANDLE, seen mid-run on this PC). The handle
 * is released without waiting for WOM_CLOSE; queued buffers are dropped and their tags returned in dropped[] (the
 * caller reports them released). The completion event survives for the reopen, and is signaled. */
static unsigned wave_output_abandon(WaveOutput *output,uint64_t *dropped,unsigned capacity) {
 unsigned n=0;
 for(unsigned i=0;i<output->count && n<capacity;i++) dropped[n++]=output->buffers[(output->head+i)%32].tag;
 if(output->handle) {wave_api.reset(output->handle);wave_api.close(output->handle);}
 for(unsigned i=0;i<32;i++) free(output->buffers[i].header.lpData);
 HANDLE completion=output->completion;
 if(output->closed)CloseHandle(output->closed);
 memset(output,0,sizeof(*output));
 output->completion=completion;
 if(completion)SetEvent(completion);
 return n;
}
static MMRESULT wave_output_close(WaveOutput *output) {
 if(output->handle) {
  output->ready=0;output->running=0;
  MMRESULT result=wave_api.reset(output->handle);if(result)return result;
  for(unsigned i=0;i<32;i++) {
   WaveBuffer *buffer=&output->buffers[i];if(!buffer->header.lpData)continue;
   if(buffer->header.dwFlags&WHDR_PREPARED) {
    result=wave_api.unprepare(output->handle,&buffer->header,sizeof(WAVEHDR));if(result)return result;
   }
   free(buffer->header.lpData);memset(buffer,0,sizeof(*buffer));
  }
  result=wave_api.close(output->handle);if(result)return result;
  output->handle=NULL;
 }
 /* Retain callback context/events on timeout so a delayed callback cannot use
  * freed memory or recycled handles. The owner must retain output and retry. */
 if(output->closed && WaitForSingleObject(output->closed,3000)!=WAIT_OBJECT_0)return MMSYSERR_ERROR;
 if(output->completion)CloseHandle(output->completion);
 if(output->closed)CloseHandle(output->closed);
 memset(output,0,sizeof(*output));return MMSYSERR_NOERROR;
}
