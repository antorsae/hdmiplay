/*
Copyright (c) 2012, Broadcom Europe Ltd
Copyright (c) 2015, Amanogawa Audio Labo
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
Original file is /opt/vc/src/hello_pi/hello_audio/aucdio.c (Raspberry pi)
Modified by Amanogawa Audio Lab
*/

// hdmi_play2.c version 0.12

// Audio output demo using OpenMAX IL though the ilcient helper library

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <semaphore.h>
#include <pthread.h>

#include "bcm_host.h"
#include "ilclient.h"
#include "interface/vmcs_host/vc_hdmi.h"

#define OUT_CHANNELS(num_channels) ((num_channels) > 4 ? 8: (num_channels) > 2 ? 4: (num_channels))

#ifndef countof
#define countof(arr) (sizeof(arr) / sizeof(arr[0]))
#endif

#define BUFFER_SIZE_SAMPLES 8192
#define BN 8
#define OUT_EN 0
#define IN_EN 1
#define SLEEPTIME 50000

typedef enum { false, true } bool;
typedef int int32_t;

typedef struct {
  sem_t sema;
  ILCLIENT_T *client;
  COMPONENT_T *audio_render;
  COMPONENT_T *list[2];
  OMX_BUFFERHEADERTYPE *user_buffer_list; // buffers owned by the client
  uint32_t num_buffers;
  uint32_t bytes_per_sample;
} AUDIOPLAY_STATE_T;

uint8_t gbuf[BN][BUFFER_SIZE_SAMPLES * 8 * 4]={};
volatile int gbuf_st[BN]={};
pthread_mutex_t mutex[BN];  // Mutex
int _bytes_per_sample;

static void SetAudioProps(bool stream_channels, uint32_t channel_map)
{
  char command[80], response[80];

  sprintf(command, "hdmi_stream_channels %d", stream_channels ? 1 : 0);
  vc_gencmd(response, sizeof response, command);

  sprintf(command, "hdmi_channel_map 0x%08x", channel_map);
  vc_gencmd(response, sizeof response, command);

}


static void input_buffer_callback(void *data, COMPONENT_T *comp)
{
  // do nothing - could add a callback to the user
  // to indicate more buffers may be available.
}

int32_t audioplay_create(AUDIOPLAY_STATE_T **handle,
    uint32_t sample_rate,
    uint32_t num_channels,
    uint32_t bit_depth,
    uint32_t num_buffers,
    uint32_t buffer_size)
{
  uint32_t bytes_per_sample = (bit_depth * OUT_CHANNELS(num_channels)) >> 3;
  int32_t ret = -1;

  *handle = NULL;

  // basic sanity check on arguments
  if(sample_rate >= 8000 && sample_rate <= 192000 &&
      (num_channels >= 1 && num_channels <= 8) &&
      (bit_depth == 16 || bit_depth == 24 || bit_depth == 32) &&
      num_buffers > 0 &&
      buffer_size >= bytes_per_sample)
  {
    // buffer lengths must be 16 byte aligned for VCHI
    int size = (buffer_size + 15) & ~15;
    AUDIOPLAY_STATE_T *st;

    // buffer offsets must also be 16 byte aligned for VCHI
    st = calloc(1, sizeof(AUDIOPLAY_STATE_T));

    if(st)
    {
      OMX_ERRORTYPE error;
      OMX_PARAM_PORTDEFINITIONTYPE param;
      OMX_AUDIO_PARAM_PCMMODETYPE pcm;
      int32_t s;

      ret = 0;
      *handle = st;

      // create and start up everything
      s = sem_init(&st->sema, 0, 1);
      assert(s == 0);

      st->bytes_per_sample = bytes_per_sample;
      st->num_buffers = num_buffers;

      st->client = ilclient_init();
      assert(st->client != NULL);

      ilclient_set_empty_buffer_done_callback(st->client, input_buffer_callback, st);

      error = OMX_Init();
      assert(error == OMX_ErrorNone);

      ilclient_create_component(st->client, &st->audio_render, "audio_render", ILCLIENT_ENABLE_INPUT_BUFFERS | ILCLIENT_DISABLE_ALL_PORTS);
      assert(st->audio_render != NULL);

      st->list[0] = st->audio_render;

      // set up the number/size of buffers
      memset(&param, 0, sizeof(OMX_PARAM_PORTDEFINITIONTYPE));
      param.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
      param.nVersion.nVersion = OMX_VERSION;
      param.nPortIndex = 100;

      error = OMX_GetParameter(ILC_GET_HANDLE(st->audio_render), OMX_IndexParamPortDefinition, &param);
      assert(error == OMX_ErrorNone);

      param.nBufferSize = size;
      param.nBufferCountActual = num_buffers;

      error = OMX_SetParameter(ILC_GET_HANDLE(st->audio_render), OMX_IndexParamPortDefinition, &param);
      assert(error == OMX_ErrorNone);

      // set the pcm parameters
      memset(&pcm, 0, sizeof(OMX_AUDIO_PARAM_PCMMODETYPE));
      pcm.nSize = sizeof(OMX_AUDIO_PARAM_PCMMODETYPE);
      pcm.nVersion.nVersion = OMX_VERSION;
      pcm.nPortIndex = 100;
      pcm.nChannels = OUT_CHANNELS(num_channels);
      pcm.eNumData = OMX_NumericalDataSigned;
      pcm.eEndian = OMX_EndianLittle;
      pcm.nSamplingRate = sample_rate;
      pcm.bInterleaved = OMX_TRUE;
      pcm.nBitPerSample = bit_depth;
      pcm.ePCMMode = OMX_AUDIO_PCMModeLinear;

      switch(num_channels) {
        case 1:
            pcm.eChannelMapping[0] = OMX_AUDIO_ChannelCF;
            break;
         case 8:
            pcm.eChannelMapping[0] = OMX_AUDIO_ChannelLF;
            pcm.eChannelMapping[1] = OMX_AUDIO_ChannelRF;
            pcm.eChannelMapping[3] = OMX_AUDIO_ChannelCF;
            pcm.eChannelMapping[2] = OMX_AUDIO_ChannelLFE;
            pcm.eChannelMapping[4] = OMX_AUDIO_ChannelLR;
            pcm.eChannelMapping[5] = OMX_AUDIO_ChannelRR;
            pcm.eChannelMapping[6] = OMX_AUDIO_ChannelLS;
            pcm.eChannelMapping[7] = OMX_AUDIO_ChannelRS;
            break;
         case 4:
            pcm.eChannelMapping[0] = OMX_AUDIO_ChannelLF;
            pcm.eChannelMapping[1] = OMX_AUDIO_ChannelRF;
            pcm.eChannelMapping[2] = OMX_AUDIO_ChannelLR;
            pcm.eChannelMapping[3] = OMX_AUDIO_ChannelRR;
            break;
         case 2:
            pcm.eChannelMapping[0] = OMX_AUDIO_ChannelLF;
            pcm.eChannelMapping[1] = OMX_AUDIO_ChannelRF;
            break;
      }

      error = OMX_SetParameter(ILC_GET_HANDLE(st->audio_render), OMX_IndexParamAudioPcm, &pcm);
      assert(error == OMX_ErrorNone);

      ilclient_change_component_state(st->audio_render, OMX_StateIdle);
      if(ilclient_enable_port_buffers(st->audio_render, 100, NULL, NULL, NULL) < 0)
      {
        // error
        ilclient_change_component_state(st->audio_render, OMX_StateLoaded);
        ilclient_cleanup_components(st->list);

        error = OMX_Deinit();
        assert(error == OMX_ErrorNone);

        ilclient_destroy(st->client);

        sem_destroy(&st->sema);
        free(st);
        *handle = NULL;
        return -1;
      }

      ilclient_change_component_state(st->audio_render, OMX_StateExecuting);
    }
  }

  return ret;
}

int32_t audioplay_delete(AUDIOPLAY_STATE_T *st)
{
  OMX_ERRORTYPE error;

  ilclient_change_component_state(st->audio_render, OMX_StateIdle);

  error = OMX_SendCommand(ILC_GET_HANDLE(st->audio_render), OMX_CommandStateSet, OMX_StateLoaded, NULL);
  assert(error == OMX_ErrorNone);

  ilclient_disable_port_buffers(st->audio_render, 100, st->user_buffer_list, NULL, NULL);
  ilclient_change_component_state(st->audio_render, OMX_StateLoaded);
  ilclient_cleanup_components(st->list);

  error = OMX_Deinit();
  assert(error == OMX_ErrorNone);

  ilclient_destroy(st->client);

  sem_destroy(&st->sema);
  free(st);

  return 0;
}

uint8_t *audioplay_get_buffer(AUDIOPLAY_STATE_T *st)
{
  OMX_BUFFERHEADERTYPE *hdr = NULL;

  hdr = ilclient_get_input_buffer(st->audio_render, 100, 0);

  if(hdr)
  {
    // put on the user list
    sem_wait(&st->sema);

    hdr->pAppPrivate = st->user_buffer_list;
    st->user_buffer_list = hdr;

    sem_post(&st->sema);
  }

  return hdr ? hdr->pBuffer : NULL;
}

int32_t audioplay_play_buffer(AUDIOPLAY_STATE_T *st,
    uint8_t *buffer,
    uint32_t length)
{
  OMX_BUFFERHEADERTYPE *hdr = NULL, *prev = NULL;
  int32_t ret = -1;

  if(length % st->bytes_per_sample)
    return ret;

  sem_wait(&st->sema);

  // search through user list for the right buffer header
  hdr = st->user_buffer_list;
  while(hdr != NULL && hdr->pBuffer != buffer && hdr->nAllocLen < length)
  {
    prev = hdr;
    hdr = hdr->pAppPrivate;
  }

  if(hdr) // we found it, remove from list
  {
    ret = 0;
    if(prev)
      prev->pAppPrivate = hdr->pAppPrivate;
    else
      st->user_buffer_list = hdr->pAppPrivate;
  }

  sem_post(&st->sema);

  if(hdr)
  {
    OMX_ERRORTYPE error;

    hdr->pAppPrivate = NULL;
    hdr->nOffset = 0;
    hdr->nFilledLen = length;

    error = OMX_EmptyThisBuffer(ILC_GET_HANDLE(st->audio_render), hdr);
    assert(error == OMX_ErrorNone);
  }

  return ret;
}

int32_t audioplay_set_dest(AUDIOPLAY_STATE_T *st, const char *name)
{
  int32_t success = -1;
  OMX_CONFIG_BRCMAUDIODESTINATIONTYPE ar_dest;

  if (name && strlen(name) < sizeof(ar_dest.sName))
  {
    OMX_ERRORTYPE error;
    memset(&ar_dest, 0, sizeof(ar_dest));
    ar_dest.nSize = sizeof(OMX_CONFIG_BRCMAUDIODESTINATIONTYPE);
    ar_dest.nVersion.nVersion = OMX_VERSION;
    strcpy((char *)ar_dest.sName, name);

    error = OMX_SetConfig(ILC_GET_HANDLE(st->audio_render), OMX_IndexConfigBrcmAudioDestination, &ar_dest);
    assert(error == OMX_ErrorNone);
    success = 0;
  }

  return success;
}


uint32_t audioplay_get_latency(AUDIOPLAY_STATE_T *st)
{
  OMX_PARAM_U32TYPE param;
  OMX_ERRORTYPE error;

  memset(&param, 0, sizeof(OMX_PARAM_U32TYPE));
  param.nSize = sizeof(OMX_PARAM_U32TYPE);
  param.nVersion.nVersion = OMX_VERSION;
  param.nPortIndex = 100;

  error = OMX_GetConfig(ILC_GET_HANDLE(st->audio_render), OMX_IndexConfigAudioRenderingLatency, &param);
  assert(error == OMX_ErrorNone);

  return param.nU32;
}


#define CTTW_SLEEP_TIME 10
#define MIN_LATENCY_TIME 20

static const char *audio_dest[] = {"local", "hdmi"};
void play_api_test(int samplerate, int bitdepth, int nchannels, int dest, uint32_t channel_map)
{
  AUDIOPLAY_STATE_T *st;
  int32_t ret;
  int buffer_size = (BUFFER_SIZE_SAMPLES * bitdepth * OUT_CHANNELS(nchannels))>>3;
  int bn=0;
  uint8_t *p;
  assert(dest == 0 || dest == 1);

  ret = audioplay_create(&st, samplerate, nchannels, bitdepth, 10, buffer_size);
  assert(ret == 0);

  SetAudioProps(0, channel_map);

  ret = audioplay_set_dest(st, audio_dest[dest]);
  assert(ret == 0);
  // iterate for 5 seconds worth of packets
  while(1)
  {
    uint8_t *buf;
    uint32_t latency;

    while((buf = audioplay_get_buffer(st)) == NULL)
      usleep(10*1000);
    p=(uint8_t *)buf;

    pthread_mutex_lock( &mutex[bn] );
    // fill the buffer FROM gbuf[bn]
    if (gbuf_st[bn] != OUT_EN)
      return;
    memcpy (buf,gbuf[bn],buffer_size);
    /*int i,j;
    printf("_bytes_per_sample: %d GB_SIZE %d\n", _bytes_per_sample, GB_SIZE);
    for (i=0; i<GB_SIZE/_bytes_per_sample; i++){
      printf("%d ", i);
      for (j=0;j<_bytes_per_sample;j++)
        p[i*_bytes_per_sample+j]=0;//gbuf[bn][i*_bytes_per_sample+j];
    }*/
    // try and wait for a minimum latency time (in ms) before
    // sending the next packet
    while((latency = audioplay_get_latency(st)) > (samplerate * (MIN_LATENCY_TIME + CTTW_SLEEP_TIME) / 1000))
      usleep(CTTW_SLEEP_TIME*1000);

    ret = audioplay_play_buffer(st, buf, buffer_size);
    gbuf_st[bn]=IN_EN;
    pthread_mutex_unlock( &mutex[bn] );
    if (++bn == BN)
      bn=0;
    assert(ret == 0);
  }

  audioplay_delete(st);
}

void* in_thread(void)
{
    int bn=0, k;
  while(1){
    int ret_n;
    pthread_mutex_lock( &mutex[bn] );
    if (gbuf_st[bn] != IN_EN){
      pthread_mutex_unlock( &mutex[bn] );
      usleep(SLEEPTIME);
      continue;
    }
    ret_n=fread(gbuf[bn],_bytes_per_sample,BUFFER_SIZE_SAMPLES * 8, stdin);
    if (ret_n < BUFFER_SIZE_SAMPLES*8){
      int  j;
      for (j=ret_n; j<BUFFER_SIZE_SAMPLES*8; j++){
        for (k=0;k < _bytes_per_sample;k++)
          gbuf[bn][j+k]=0;
      }
      gbuf_st[bn]=OUT_EN;
      pthread_mutex_unlock( &mutex[bn] );
      return;
    }
    gbuf_st[bn]=OUT_EN;
    pthread_mutex_unlock( &mutex[bn] );
    if (++bn == BN)
      bn=0;
  }
}
void printhelp(void)
{
  char s[]="\n"
    "This program receives 8ch data from stdin and output to HDMI\n"
    "(Raspberry Pi)  "
    "Using OpenMAX IL, not ALSA\n\n"
    "Usage: hdmiplay.bin rate bitdepth [mapping]\n\n"
    "        rate in hz"
    "        bitdepht 16 24 32"
    "        mapping 01234567"
    "Example: sox test.wav -t .s16 - | brutefir 3way.conf | hdmi_play2.bin 44100 24\n\n";

  printf("%s",s);
  return;
}

int main (int argc, char **argv)
{

  int audio_dest = 1; // 0=headphones, 1=hdmi   
  int channels = 8;
  int samplerate;
  int bitdepth ;

  bcm_host_init();

  if (argc < 3){ 
    printhelp();
    return 0;
  }

  samplerate = atoi(argv[1]); 
  bitdepth=atoi(argv[2]);
  _bytes_per_sample = bitdepth >> 3;
  int i;
  uint32_t channel_map = 0xfac688; // 111 110 101 100 011 010 001 000
  if (argc == 4) {
    int map_len = strlen(argv[3]);
    channel_map = 0;
    for (i=0; i < map_len; i++) {
      char c = argv[3][map_len-1-i];
      channel_map <<= 3;
      channel_map |= (c - '0') & 0x7;  
      
    }
  }
  channel_map |= 0x13000000;
  printf("channel_map: %08x\n", channel_map);
  //sleep (1); //if nessesary

  //in_thread
  pthread_t thread1;
  if(pthread_create(&thread1, NULL, (void *)in_thread, NULL) < 0){
    perror("pthread_vreate error");
    return 0;
  }


  // main loop

  play_api_test(samplerate, bitdepth, channels, audio_dest, channel_map);


  return 0;
}
