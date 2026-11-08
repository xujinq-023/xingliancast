// fdk-aac 独立复现：精确复刻 sender audio.h OpenEncoder + EncodePcm 序列
// 目的：隔离验证 AdjustEncSettings NULL-channelMode 崩溃是 fdk 内在还是 sender 集成
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <aacenc_lib.h>

static HANDLE_AACENCODER enc = NULL;
static unsigned char obuf[4096];

int main(void) {
  AACENC_ERROR e;
  printf("step1: aacEncOpen\n"); fflush(stdout);
  e = aacEncOpen(&enc, 0, 2);
  if (e != AACENC_OK) { printf("open fail %d\n", e); return 1; }

  printf("step2: SetParams\n"); fflush(stdout);
  aacEncoder_SetParam(enc, AACENC_AOT, (int)AOT_AAC_LC);
  aacEncoder_SetParam(enc, AACENC_BITRATE, 128u * 1000u);
  aacEncoder_SetParam(enc, AACENC_SAMPLERATE, 48000u);
  {   /* 候选修复：默认 userChannelMode=MODE_UNKNOWN 导致 AdjustEncSettings 空指针 */
    AACENC_ERROR e2 = aacEncoder_SetParam(enc, AACENC_CHANNELMODE, (unsigned)2 /*MODE_2*/);
    printf("  SetParam CHANNELMODE err=%d\n", e2); fflush(stdout);
  }
  aacEncoder_SetParam(enc, AACENC_TRANSMUX, (int)TT_MP4_ADTS);

  printf("step3: start call (NULL bufs)\n"); fflush(stdout);
  e = aacEncEncode(enc, NULL, NULL, NULL, NULL);
  printf("start call err=%d\n", e); fflush(stdout);
  if (e != AACENC_OK) return 2;

  static short pcm[1024 * 2];
  memset(pcm, 0, sizeof(pcm));
  for (int i = 0; i < 3000; i++) {
    if (i % 300 == 299) { for (int k = 0; k < 2048; k++) pcm[k] = (short)(k * 7 + i); }
    AACENC_BufDesc ind, outd;
    AACENC_InArgs ia;
    AACENC_OutArgs oa;
    void* ib = pcm; void* ob = obuf;
    INT isz = 1024 * 2 * (INT)sizeof(short); INT osz = 2048;
    INT it = IN_AUDIO_DATA, ot = OUT_BITSTREAM_DATA;
    INT iel = 2, oel = 1;
    ind.numBufs = 1; ind.bufs = &ib; ind.bufferIdentifiers = &it; ind.bufSizes = &isz; ind.bufElSizes = &iel;
    outd.numBufs = 1; outd.bufs = &ob; outd.bufferIdentifiers = &ot; outd.bufSizes = &osz; outd.bufElSizes = &oel;
    memset(&ia, 0, sizeof ia); ia.numInSamples = 1024 * 2;   // = frames*ech，与 sender 完全一致
    memset(&oa, 0, sizeof oa);
    e = aacEncEncode(enc, &ind, &outd, &ia, &oa);
    if (e != AACENC_OK) { printf("frame %d err=%d\n", i, e); fflush(stdout); return 3; }
    if ((i & 0xFF) == 0) { printf("frame %4d ok, bytes=%d\n", i, oa.numOutBytes); fflush(stdout); }
    Sleep(8);   // 模拟略快于实时的喂帧节奏
  }
  printf("ALL 3000 FRAMES OK\n"); fflush(stdout);
  aacEncClose(&enc);
  return 0;
}
