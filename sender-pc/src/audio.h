// M3 音频链路: WASAPI loopback(共享混音) → float32→s16 → fdk-aac(AAC-LC) → ADTS 帧回调
// protocol/pjs-spec.md §3: type=AUDIO codec=1, 一包一 AAC(ADTS) 帧。独立线程，事件驱动。
//
// 为何不用 Windows 自带 MF AAC：确诊三份全量 dump 显示 mfAACEnc.dll 在进程级累计
// ~150 个编码批(≈3.2s 音频内容)后自身 fastfail(0xc0000409)/堆损坏，与实例年龄、
// 时间戳连续性、重建频率均无关——换实例救不了（它是 DLL 内全局计数）。
// fdk-aac(Fraunhofer，静态链接)无 COM、无状态上限，且与乐播同为 Fraunhofer 编码核。
#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <aacenc_lib.h>
#include <FDK_audio.h>
#include <cstdio>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>
#include <wrl/client.h>
#include "logging.h"
using Microsoft::WRL::ComPtr;

namespace aud {

class AudioPipe {
 public:
  using Fn = std::function<void(const uint8_t* frame_with_adts, size_t n, uint64_t ts_us)>;

  // kbps: AAC 码率。返回 false 表示无默认输出设备/编码器不可用（调用方可降级为纯视频）。
  bool Start(int kbps, const Fn& on_frame) {
    on_ = on_frame;
    stop_ = false; started_ = false; err_ = S_OK;   // 支持 Stop 后再 Start（界面实时开关声音）
    thr_ = std::thread([this, kbps] { Run(kbps); });
    return true;   // 设备错误在 Run 内通过 started_/err_ 汇报
  }
  bool started() const { return started_; }
  HRESULT err() const { return err_; }
  void Stop() {
    stop_ = true;
    if (thr_.joinable()) thr_.join();
  }
  ~AudioPipe() { Stop(); }

 private:
  // ---------- AAC 编码器（fdk-aac，同步纯 CPU） ----------
  bool OpenEncoder(int kbps) {
    HANDLE_AACENCODER h = nullptr;
    AACENC_ERROR e = aacEncOpen(&h, 0, ech_);   // 2ch（Run 已把采集重采样到 erate_ 立体声）
    if (e != AACENC_OK || !h) { err_ = (HRESULT)e; return false; }
    aacEncoder_SetParam(h, AACENC_AOT, (int)AOT_AAC_LC);
    aacEncoder_SetParam(h, AACENC_BITRATE, (UINT)kbps * 1000u);
    aacEncoder_SetParam(h, AACENC_SAMPLERATE, erate_);
    // 关键：fdk v3.0.0 默认 userChannelMode=MODE_UNKNOWN，而 AdjustEncSettings
    // 对查表结果无 NULL 防护（aacenc_lib.cpp L892-893）→ 不设 CHANNELMODE
    // 则首次 aacEncEncode(含 start call) 必现 AV read addr=4（已独立复现）。
    e = aacEncoder_SetParam(h, AACENC_CHANNELMODE, (ech_ == 2) ? 2u /*MODE_2*/ : 1u /*MODE_1*/);
    if (e != AACENC_OK) { aacEncClose(&h); err_ = (HRESULT)e; return false; }
    aacEncoder_SetParam(h, AACENC_TRANSMUX, (UINT)TT_MP4_ADTS);   // 直接输出 ADTS 帧
    // 启动流（NULL 缓冲 = 初始化，吃掉编码器 priming 延迟）
    e = aacEncEncode(h, nullptr, nullptr, nullptr, nullptr);
    if (e != AACENC_OK) { aacEncClose(&h); err_ = (HRESULT)e; return false; }
    enc_ = h;
    Logf("[audio] fdk-aac 就绪: AAC-LC %uHz/%uch %dkbps → ADTS\n", erate_, ech_, kbps);
    return true;
  }

  // 一帧 = 1024 立体声样本 s16；fdk 稳定态下输入 1 帧 ⇒ 输出恰好 1 个 ADTS 帧
  bool EncodePcm(const int16_t* pcm, size_t frames, uint64_t ts_us) {
    if (!enc_ || !pcm || !frames) return false;
    INT in_bytes = (INT)(frames * ech_ * 2);
    void* inb = (void*)pcm; void* outb = obuf_.data();
    INT iid = IN_AUDIO_DATA, isz = in_bytes, iel = 2;
    INT oid = OUT_BITSTREAM_DATA, osz = (INT)obuf_.size(), oel = 1;
    AACENC_BufDesc ind{ 1, &inb, &iid, &isz, &iel };
    AACENC_BufDesc outd{ 1, &outb, &oid, &osz, &oel };
    AACENC_InArgs ia{ (INT)(frames * ech_), 0 };
    AACENC_OutArgs oa{};
    AACENC_ERROR e = aacEncEncode(enc_, &ind, &outd, &ia, &oa);
    if (e != AACENC_OK) {
      if (++enc_err_ <= 3) Logf("[audio] fdk 编码失败 err=0x%x\n", (unsigned)e);
      return false;
    }
    if (oa.numOutBytes > 0) { on_(obuf_.data(), (size_t)oa.numOutBytes, ts_us); enc_err_ = 0; }
    return true;
  }

  // ---------- WASAPI loopback 采集 ----------
  void Run(int kbps) {
    HRESULT hr0 = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    OnScopeExit co{ [hr0] { if (SUCCEEDED(hr0)) CoUninitialize(); } };
    if (FAILED(hr0)) { err_ = hr0; return; }
    OnScopeExit fin{ [this] { if (enc_) { aacEncClose(&enc_); enc_ = nullptr; } } };
    if (!OpenEncoder(kbps)) { /* 编码器不可用：继续采集但不喂音频，由 err_ 汇报 */ }

    ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> cli; ComPtr<IAudioCaptureClient> cap;
    hr0 = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (SUCCEEDED(hr0)) hr0 = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (SUCCEEDED(hr0)) hr0 = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cli);
    // 共享模式 loopback 不支持格式转换（AUTOCONVERTPCM 仅独占模式有效）→ 按设备 mix 格式直采，自行下混+重采样
    WAVEFORMATEX* mwf = nullptr;
    if (SUCCEEDED(hr0)) hr0 = cli->GetMixFormat(&mwf);
    if (SUCCEEDED(hr0)) hr0 = cli->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mwf, nullptr);
    UINT32 mrate = mwf ? mwf->nSamplesPerSec : 48000, mch = mwf ? mwf->nChannels : 2;
    bool mflt = mwf && (mwf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (mwf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         ((WAVEFORMATEXTENSIBLE*)mwf)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    if (mch == 0) mch = 2;
    if (mwf) CoTaskMemFree(mwf);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(hr0)) hr0 = cli->SetEventHandle(ev);   // SDK 真名 SetEventHandle（非 MSDN 的 SetEventCallback）
    if (FAILED(hr0)) { hr0 = S_OK; ev = nullptr; }       // 后备：无事件则 10ms 轮询
    if (SUCCEEDED(hr0)) hr0 = cli->GetService(IID_PPV_ARGS(&cap));
    UINT32 bufframes = 0;
    if (SUCCEEDED(hr0)) hr0 = cli->GetBufferSize(&bufframes);
    if (FAILED(hr0)) { err_ = hr0; return; }

    if (FAILED(cli->Start())) { err_ = E_FAIL; return; }
    started_ = true;

    LARGE_INTEGER qpf{}; QueryPerformanceFrequency(&qpf);
    auto qpc_us = [&] { LARGE_INTEGER q; QueryPerformanceCounter(&q);
                        return (uint64_t)q.QuadPart * 1000000 / (uint64_t)qpf.QuadPart; };
    const double rstep = (double)mrate / (double)erate_;   // 重采样步进
    std::vector<int16_t> pcm; size_t carry = 0;   // 1024 帧对齐缓存
    std::vector<float> fbuf;  double rpos = 0.0;           // 采集率立体声 float 缓存 + 消费位置
    auto clamp16 = [](float v) { v = v < -1.f ? -1.f : (v > 1.f ? 1.f : v); return (int16_t)(v * 32767.f); };
    auto flush_resample = [&] {
      size_t total = fbuf.size() / 2;
      if (total < 2) return;
      double pos = rpos;
      while (pos + 1.0 < (double)total) {
        size_t i = (size_t)pos; float a = (float)(pos - i);
        float l = fbuf[2*i] * (1.f-a) + fbuf[2*i+2] * a;
        float r = fbuf[2*i+1] * (1.f-a) + fbuf[2*i+3] * a;
        if (ech_ == 2) { pcm.push_back(clamp16(l)); pcm.push_back(clamp16(r)); }
        else pcm.push_back(clamp16((l + r) * 0.5f));
        pos += rstep;
      }
      size_t keep = (size_t)pos;
      if (keep > 0) fbuf.erase(fbuf.begin(), fbuf.begin() + (long)keep * 2);
      rpos = pos - (double)keep;
    };
    uint64_t nts = 0;                             // 音频流连续时间戳(µs)
    while (!stop_) {
      if (ev) WaitForSingleObject(ev, 250); else Sleep(10);
      UINT32 np = 0; bool got = false;
      while (SUCCEEDED(cap->GetNextPacketSize(&np)) && np > 0) {
        got = true;
        BYTE* data = nullptr; UINT32 nf = 0; DWORD flg = 0;
        if (FAILED(cap->GetBuffer(&data, &nf, &flg, nullptr, nullptr))) break;
        size_t old = fbuf.size();
        fbuf.resize(old + (size_t)nf * 2);
        float* dst = fbuf.data() + old;
        bool silent = (flg & AUDCLNT_BUFFERFLAGS_SILENT) || !data;
        for (UINT32 k = 0; k < nf; k++) {
          float l = 0.f, r = 0.f;
          if (!silent) {
            if (mflt) { const float* s = (const float*)data; l = s[k*mch]; r = mch >= 2 ? s[k*mch+1] : l; }
            else { const int16_t* s = (const int16_t*)data; l = s[k*mch] / 32768.f; r = mch >= 2 ? s[k*mch+1] / 32768.f : l; }
          }
          dst[2*k] = l; dst[2*k+1] = r;
        }
        cap->ReleaseBuffer(nf);
        flush_resample();
      }
      if (!got) {   // 无采集数据 → 补 40ms 静音保编码时钟（无声时 AAC 连续，TV 端队列不枯）
        pcm.insert(pcm.end(), (size_t)(0.04 * erate_) * ech_, (int16_t)0);
      }
      uint64_t qnow = qpc_us();
      // 时间戳严格连续 +21.3ms/批（仅首批取 qnow），无声期 ts 落后实时无碍
      // （硬解按流序播放；A/V sync 由 M3.1 统一管理）。
      if (!nts) nts = qnow;
      while (pcm.size() - carry >= 1024 * ech_) {
        if (!enc_) {                              // 编码器不可用：丢弃批但保持时间戳前进
          carry += 1024 * ech_;
          nts += 1024ULL * 1000000 / erate_;
          continue;
        }
        EncodePcm(pcm.data() + carry, 1024, nts);
        nts += 1024ULL * 1000000 / erate_;
        carry += 1024 * ech_;
      }
      if (carry > 4096 * ech_) { pcm.erase(pcm.begin(), pcm.begin() + (long)carry); carry = 0; }
    }
    cli->Stop();
  }

  struct OnScopeExit { std::function<void()> f; ~OnScopeExit() { f(); } };

  Fn on_;
  HANDLE_AACENCODER enc_ = nullptr;
  int enc_err_ = 0;
  std::vector<uint8_t> obuf_ = std::vector<uint8_t>(2048);   // ADTS 输出缓冲（单帧 AAC 上限 << 2048）
  UINT32 erate_ = 48000, ech_ = 2;      // 编码版式固定 48k/立体声（采集端重采样对齐）
  std::thread thr_;
  std::atomic<bool> stop_{ false }, started_{ false };
  HRESULT err_ = S_OK;
};
} // namespace aud
