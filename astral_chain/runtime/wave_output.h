/* Windows PCM transport. No guest memory, IPC, synthetic playback clock or
 * fallback-success path. Serialize control calls and keep WaveOutput at a stable
 * address until close ends. A failed close must be retried before reuse. */
#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <math.h>
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
 union { WAVEFORMATEX format; WAVEFORMATEXTENSIBLE format_ext; };  /* what the device is given */
 unsigned stream_channels,stream_block;  /* what the game sends (6: a 5.1 mix) */
 int fold;  /* the 5.1 mix is folded down to stereo here (the device has fewer speakers) */
 WaveBuffer buffers[32];
 unsigned head,count,running,ready;
 uint64_t queued_bytes,position_epoch;
 uint32_t last_position;
 double play_end,starved;  /* (QPC seconds) when the queued sound runs out; total time the queue was dry */
 double last_append,queued_then;  /* (underrun reports) previous append, and the sound queued before it */
 double owed,skipped;  /* sound still to skip after an underrun; total skipped */
 uint64_t appended;  /* frames written to the device since it was opened */
} WaveOutput;

static void CALLBACK wave_output_callback(HWAVEOUT wave,UINT message,DWORD_PTR context,DWORD_PTR first,DWORD_PTR second) {
 (void)wave;(void)first;(void)second;
 WaveOutput *output=(WaveOutput*)context;
 /* The callback must not call waveform APIs or touch guest/queue memory. */
 if(message==WOM_DONE)SetEvent(output->completion);
 if(message==WOM_CLOSE)SetEvent(output->closed);
}

/* The default sound device's channel count (its mix format), asked on a thread of its own so the caller's COM state is
 * not touched; 0 when unknown. Windows "5.1" or "7.1" speakers give 6 or 8; headphones and stereo speakers 2 (also
 * with Windows Sonic or Dolby Atmos for Headphones on, which keep a stereo mix format). */
#include <mmdeviceapi.h>
#include <audioclient.h>
static volatile LONG wave_speakers;
static DWORD WINAPI wave_device_channels_thread(void *unused) {
 (void)unused;
 HMODULE ole=LoadLibraryExW(L"ole32.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!ole) return 0;
 HRESULT (WINAPI *init)(void*,DWORD)=(HRESULT (WINAPI*)(void*,DWORD))GetProcAddress(ole,"CoInitializeEx");
 void (WINAPI *uninit)(void)=(void (WINAPI*)(void))GetProcAddress(ole,"CoUninitialize");
 HRESULT (WINAPI *create)(REFCLSID,void*,DWORD,REFIID,void**)=(HRESULT (WINAPI*)(REFCLSID,void*,DWORD,REFIID,void**))GetProcAddress(ole,"CoCreateInstance");
 void (WINAPI *taskfree)(void*)=(void (WINAPI*)(void*))GetProcAddress(ole,"CoTaskMemFree");
 if(init && uninit && create && taskfree && SUCCEEDED(init(NULL,COINIT_MULTITHREADED))) {
  static const GUID clsid_enum={0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
  static const GUID iid_enum={0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
  static const GUID iid_client={0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
  IMMDeviceEnumerator *e=NULL;IMMDevice *d=NULL;IAudioClient *c=NULL;WAVEFORMATEX *mix=NULL;
  if(SUCCEEDED(create(&clsid_enum,NULL,CLSCTX_ALL,&iid_enum,(void**)&e)) &&
     SUCCEEDED(e->lpVtbl->GetDefaultAudioEndpoint(e,eRender,eConsole,&d)) &&
     SUCCEEDED(d->lpVtbl->Activate(d,&iid_client,CLSCTX_ALL,NULL,(void**)&c)) &&
     SUCCEEDED(c->lpVtbl->GetMixFormat(c,&mix)) && mix)
   InterlockedExchange(&wave_speakers,mix->nChannels);
  if(mix) taskfree(mix);
  if(c) c->lpVtbl->Release(c);
  if(d) d->lpVtbl->Release(d);
  if(e) e->lpVtbl->Release(e);
  uninit();
 }
 FreeLibrary(ole);return 0;
}
static int wave_device_channels(void) {
 InterlockedExchange(&wave_speakers,0);
 HANDLE t=CreateThread(NULL,0,wave_device_channels_thread,NULL,0,NULL);
 if(!t) return 0;
 int n=WaitForSingleObject(t,3000)==WAIT_OBJECT_0 ? (int)wave_speakers : 0;
 CloseHandle(t);return n;
}

static MMRESULT wave_output_open(WaveOutput *output,unsigned rate,unsigned channels) {
 if(output->handle || output->closed)return MMSYSERR_ALLOCATED;
 HANDLE completion=output->completion;  /* kept across a reopen: the guest holds copies of it */
 if(rate<8000 || rate>192000 || !channels || channels>8)return WAVERR_BADFORMAT;
 MMRESULT result=wave_api_load();if(result)return result;
 memset(output,0,sizeof(*output));output->completion=completion;
 output->stream_channels=channels;output->stream_block=channels*2;
 unsigned device=channels;
 if(channels==6) {  /* a 5.1 mix: as it is where the device has the speakers, else folded down to stereo here */
  int speakers=wave_device_channels();
  ac_speaker_gain_init();
  char v[4];DWORD n=GetEnvironmentVariableA("AC_AUDIO_FOLD",v,sizeof(v));  /* (tests: 1 always fold, 0 never) */
  output->fold=n==1 ? v[0]=='1' : speakers<6;
  if(output->fold) device=2;
  printf("AUDIO 5.1 mix: the default device has %d channels: %s\n",speakers,
   output->fold ? "folded down to stereo here" : "sent as 5.1");
  ac_audio_device_channels=speakers;
 }
 ac_audio_mode=channels!=6 ? 0 : output->fold ? 2 : 1;
 output->format_ext=(WAVEFORMATEXTENSIBLE){0};
 output->format=(WAVEFORMATEX){.wFormatTag=WAVE_FORMAT_PCM,.nChannels=(WORD)device,
  .nSamplesPerSec=rate,.nAvgBytesPerSec=rate*device*2,.nBlockAlign=(WORD)(device*2),.wBitsPerSample=16};
 if(device>2) {  /* the game's order: front left, right, centre, LFE, side left, right */
  static const GUID pcm={0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
  output->format.wFormatTag=WAVE_FORMAT_EXTENSIBLE;output->format.cbSize=22;
  output->format_ext.Samples.wValidBitsPerSample=16;
  output->format_ext.dwChannelMask=device==6 ? 0x60fu : 0;
  output->format_ext.SubFormat=pcm;
 }
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
 if(!result) {output->running=1;output->play_end=0;}
 return result;
}
static MMRESULT wave_output_pause(WaveOutput *output) {
 if(!output->handle || !output->ready)return MMSYSERR_INVALHANDLE;
 MMRESULT result=wave_api.pause(output->handle);
 if(!result)output->running=0;
 return result;
}
static MMRESULT wave_output_position(WaveOutput *output,uint64_t *frames);
static MMRESULT wave_output_append(WaveOutput *output,uint64_t tag,const void *pcm,size_t size) {
 if(!output->handle || !output->ready)return MMSYSERR_INVALHANDLE;
 if(!pcm || !size || size>16u*1024*1024 || size%output->stream_block)return MMSYSERR_INVALPARAM;
 if(output->count==32 || output->queued_bytes+size>64u*1024*1024)return MMSYSERR_NOMEM;
 if(output->stream_channels==6) {
  /* The player's gain per speaker (dB, live from the menu), then for a device with fewer speakers the fold to stereo
   * (ITU-R BS.775: the centre and each side at -3 dB; the LFE, which the game leaves silent, dropped); clipped */
  float g[6];int flat=1;
  for(int c=0;c<6;c++) {int db=ac_speaker_gain[c];flat&=!db;g[c]=db ? powf(10.f,db/20.f) : 1.f;}
  if(output->fold || !flat) {
   static __thread int16_t *mixed;static __thread size_t mixed_cap;
   size_t frames=size/12;
   if(mixed_cap<frames) {free(mixed);mixed=malloc(frames*12);mixed_cap=mixed ? frames : 0;if(!mixed)return MMSYSERR_NOMEM;}
   const int16_t *in=(const int16_t*)pcm;
#define WAVE_CLIP(x) ((int16_t)((x)>32767.f ? 32767.f : (x)<-32768.f ? -32768.f : (x)))
   if(output->fold) {
    for(size_t i=0;i<frames;i++,in+=6) {
     float c=0.70710678f*g[2]*in[2],l=g[0]*in[0]+c+0.70710678f*g[4]*in[4],r=g[1]*in[1]+c+0.70710678f*g[5]*in[5];
     mixed[i*2]=WAVE_CLIP(l);mixed[i*2+1]=WAVE_CLIP(r);
    }
    size=frames*4;
   } else {
    for(size_t i=0;i<frames*6;i++) {float x=g[i%6]*in[i];mixed[i]=WAVE_CLIP(x);}
   }
#undef WAVE_CLIP
   pcm=mixed;
  }
 }
 /* Timing: when the queued sound runs out (play_end, QPC seconds) and whether it ran dry before this buffer. A dry
  * queue pauses the sound, which then stays behind the picture by the gap: the cutscene drift reported on a second PC
  * (the game keeps only 15-47 ms queued; its sound thread was 55-128 ms late in 140 ms frames). Gaps under 0.5 s are
  * repaid by skipping as much of the following sound (at most 0.3 s owed; AC_AUDIO_RESYNC=0: off); longer gaps are
  * the game's own pauses (loading). */
 size_t skip=0;double dump_gap=0;
 if(output->running) {
  static int resync=-1;
  if(resync<0) {char v[8];DWORD n=GetEnvironmentVariableA("AC_AUDIO_RESYNC",v,sizeof(v));resync=!(n && n<sizeof(v) && v[0]=='0');}
  LARGE_INTEGER f,q;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&q);
  double now=(double)q.QuadPart/(double)f.QuadPart;
  uint64_t played;  /* re-anchored on the device's position: the estimate alone drifts with the device clock */
  if(output->appended && !wave_output_position(output,&played)) {
   uint64_t queued=output->appended>played ? output->appended-played : 0;
   if(queued) output->play_end=now+(double)queued/output->format.nSamplesPerSec;
   else if(output->play_end>now) output->play_end=now;
  }
  if(output->play_end>0 && now>output->play_end) {
   double gap=now-output->play_end;output->starved+=gap;dump_gap=gap<5.0 ? gap : 0;
   if(resync && gap<0.5) {output->owed+=gap;if(output->owed>0.3) output->owed=0.3;}
   if(gap>0.01) printf("AUDIO underrun at %.3f s: %.1f ms (total %.1f ms); %.0f ms since the previous buffer, %.0f ms queued then; resync %s, %.1f ms skipped so far\n",
    now,gap*1e3,output->starved*1e3,(now-output->last_append)*1e3,output->queued_then*1e3,resync?"on":"off",output->skipped*1e3);
  }
  if(output->owed>0) {
   size_t owed=(size_t)(output->owed*output->format.nAvgBytesPerSec)/output->format.nBlockAlign*output->format.nBlockAlign;
   size_t most=size/4*3/output->format.nBlockAlign*output->format.nBlockAlign;  /* (the device keeps playing) */
   skip=owed<most ? owed : most;
   output->owed-=(double)skip/output->format.nAvgBytesPerSec;if(output->owed<0.0005) output->owed=0;
   output->skipped+=(double)skip/output->format.nAvgBytesPerSec;
  }
  output->queued_then=output->play_end>now ? output->play_end-now : 0;
  output->last_append=now;
  if(output->play_end<now)output->play_end=now;
  output->play_end+=(double)(size-skip)/output->format.nAvgBytesPerSec;
 }
 {  /* AC_AUDIO_DUMP=<file>: the sound as the device plays it, raw PCM: silence for the gaps of underruns, without
     * skipped sound (verification without listening) */
  static FILE *dump;static int tried;
  if(!tried) {
   tried=1;char path[MAX_PATH];DWORD n=GetEnvironmentVariableA("AC_AUDIO_DUMP",path,sizeof(path));
   if(n && n<sizeof(path)) {dump=fopen(path,"wb");
    FILETIME ft;GetSystemTimePreciseAsFileTime(&ft);  /* (wall clock of the first appended sample: A/V sync checks) */
    unsigned long long t100=((unsigned long long)ft.dwHighDateTime<<32|ft.dwLowDateTime);
    LARGE_INTEGER f,q;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&q);
    printf("AUDIO dump %s rate=%lu channels=%u bits=16 first_append_unix=%.4f qpc=%.4f\n",path,
     (unsigned long)output->format.nSamplesPerSec,output->format.nChannels,(double)(t100-116444736000000000ull)/1e7,
     (double)q.QuadPart/(double)f.QuadPart);fflush(stdout);}
  }
  if(dump) {
   static const uint8_t zero[4096];
   size_t silence=(size_t)(dump_gap*output->format.nAvgBytesPerSec)/output->format.nBlockAlign*output->format.nBlockAlign;
   while(silence) {size_t n=silence<sizeof zero ? silence : sizeof zero;fwrite(zero,1,n,dump);silence-=n;}
   fwrite((const uint8_t*)pcm+skip,1,size-skip,dump);fflush(dump);
  }
 }
 size_t length=size-skip;
 WaveBuffer *buffer=&output->buffers[(output->head+output->count)%32];
 if(buffer->header.lpData)return MMSYSERR_ALLOCATED;
 buffer->header.lpData=malloc(length);if(!buffer->header.lpData)return MMSYSERR_NOMEM;
 memcpy(buffer->header.lpData,(const uint8_t*)pcm+skip,length);buffer->header.dwBufferLength=(DWORD)length;buffer->tag=tag;
 if(app_exit_requested) memset(buffer->header.lpData,0,length);  /* (the window was closed: silent while the game exits) */
 MMRESULT result=wave_api.prepare(output->handle,&buffer->header,sizeof(WAVEHDR));
 if(!result)result=wave_api.write(output->handle,&buffer->header,sizeof(WAVEHDR));
 if(result) {
  if(buffer->header.dwFlags&WHDR_PREPARED) {
   MMRESULT cleanup=wave_api.unprepare(output->handle,&buffer->header,sizeof(WAVEHDR));
   if(cleanup) {output->count++;output->queued_bytes+=length;output->ready=0;return cleanup;}
  }
  free(buffer->header.lpData);memset(buffer,0,sizeof(*buffer));return result;
 }
 output->count++;output->queued_bytes+=length;output->appended+=length/output->format.nBlockAlign;
 return MMSYSERR_NOERROR;
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
