// M3 音频链路: WASAPI loopback(共享混音) → float32→s16 → MF AAC-LC MFT → ADTS 帧回调
// protocol/pjs-spec.md §3: type=AUDIO codec=1, 一包一 AAC(ADTS) 帧。独立线程，事件驱动。
#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <ksmedia.h>
#include <cstdio>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

namespace aud {

// 从 MF AAC 编码器裸输出组装 7B ADTS 头（MPEG-4 AAC-LC）
inline uint8_t adts_freq_idx(UINT32 rate) {
  static const UINT32 tab[] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
  for (int i = 0; i < 13; i++) if (tab[i] == rate) return (uint8_t)i;
  return 3;   // 兜底 48k
}
inline void adts_header(uint8_t* h, size_t frame_len, UINT32 rate, UINT32 ch) {
  size_t n = frame_len + 7;
  uint8_t fi = adts_freq_idx(rate), cf = (uint8_t)(ch & 0x3);
  h[0] = 0xFF; h[1] = 0xF1;                        // sync12 + MPEG-4 + layer AAC + 无CRC
  h[2] = (uint8_t)((1 << 6) | (fi << 2) | (cf >> 1));  // profile=01(LC) freq_idx + chan>>1
  h[3] = (uint8_t)(((cf & 1) << 7) | ((n >> 11) & 0x7));  // chan LSB + frame_len[10:8]
  h[4] = (uint8_t)((n >> 3) & 0xFF);               // frame_len[7:0]
  h[5] = (uint8_t)(((n & 0x7) << 5) | (0x1FF >> 7 << 3));  // frame_len[2:0] + fullness[8:7]
  h[6] = (uint8_t)(((0x1FF & 0x7F) << 1));         // fullness[6:0]=0x1FF(可变) → 0xFC
}

class AudioPipe {
 public:
  using Fn = std::function<void(const uint8_t* frame_with_adts, size_t n, uint64_t ts_us)>;

  // kbps: AAC 码率。返回 false 表示无默认输出设备/编码器不可用（调用方可降级为纯视频）。
  bool Start(int kbps, const Fn& on_frame) {
    on_ = on_frame;
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
  // ---------- AAC 编码器（同步 MFT） ----------
  // 输出版式: payload_type 0=裸 AAC / 1=ADTS / 0xFFFFFFFF=编码器默认
  static HRESULT MakeAacOut(ComPtr<IMFMediaType>& ot, int kbps, DWORD payload_type) {
    HRESULT hr = MFCreateMediaType(&ot);
    if (FAILED(hr)) return hr;
    ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    ot->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    ot->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    ot->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    ot->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    ot->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)kbps * 1000);
    if (payload_type != 0xFFFFFFFF) {
      hr = ot->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, payload_type);
      if (FAILED(hr)) return hr;
    }
    ot->SetUINT32(MF_MT_MPEG2_PROFILE, 2);   // AAC LC
    return S_OK;
  }

  bool OpenEncoder(int kbps) {
    MFT_REGISTER_TYPE_INFO tiIn{ MFMediaType_Audio, MFAudioFormat_PCM };
    MFT_REGISTER_TYPE_INFO tiOut{ MFMediaType_Audio, MFAudioFormat_AAC };
    IMFActivate** acts = nullptr; UINT32 n = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &tiIn, &tiOut, &acts, &n);
    if (SUCCEEDED(hr) && acts) {
      for (UINT32 i = 0; i < n && !mft_; i++) acts[i]->ActivateObject(IID_PPV_ARGS(&mft_));
      for (UINT32 i = 0; i < n; i++) acts[i]->Release();
      CoTaskMemFree(acts);
    }
    if (!mft_) { err_ = MF_E_NOT_FOUND; return false; }   // 无可用 AAC 编码器（枚举为准，26100 SDK 无 CLSID 头）
    auto mk_in = [&](UINT32 rate, UINT32 ch) -> ComPtr<IMFMediaType> {
      ComPtr<IMFMediaType> t; MFCreateMediaType(&t);
      t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
      t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
      t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
      t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
      t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, ch * 2);
      t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
      t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
      t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * ch * 2);
      return t;
    };
    std::vector<ComPtr<IMFMediaType>> outs;   // 编码器输出模板全枚举 + 自建 48k/2 兜底
    for (DWORD i = 0; i < 12 && outs.size() < 8; i++) {
      ComPtr<IMFMediaType> base;
      if (FAILED(mft_->GetOutputAvailableType(0, i, &base)) || !base) break;   // MFT 原生枚举（SDK 真名）
      base->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)kbps * 1000);
      outs.push_back(base);
    }
    { ComPtr<IMFMediaType> o; if (SUCCEEDED(MakeAacOut(o, kbps, 0xFFFFFFFF))) outs.push_back(o); }
    auto rate_of = [](const ComPtr<IMFMediaType>& t) { UINT32 r = 0; t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r); return r; };
    auto ch_of = [](const ComPtr<IMFMediaType>& t) { UINT32 c = 0; t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &c); return c; };
    std::stable_sort(outs.begin(), outs.end(), [&](const ComPtr<IMFMediaType>& a, const ComPtr<IMFMediaType>& b) {
      auto score = [&](const ComPtr<IMFMediaType>& t) { return (rate_of(t) == 48000 ? 4 : 0) + (ch_of(t) == 2 ? 2 : 0); };
      return score(a) > score(b);   // 48k/2ch 模板优先（免重采样/下混）
    });
    bool neg = false; HRESULT last = MF_E_INVALIDMEDIATYPE;
    for (size_t oi = 0; oi < outs.size() && !neg; oi++) {
      auto& ot = outs[oi];
      UINT32 trate = rate_of(ot), tch = ch_of(ot);
      if (!trate) trate = 48000; if (!tch) tch = 2;
      last = mft_->SetOutputType(0, ot.Get(), 0);
      if (FAILED(last)) continue;
      ComPtr<IMFMediaType> in1 = mk_in(trate, tch);   // 输入必须与输出模板同 rate/ch
      last = mft_->SetInputType(0, in1.Get(), 0);
      if (FAILED(last)) { mft_->SetOutputType(0, nullptr, 0); continue; }
      neg = true;
      pay_ = 0xFFFFFFFF; erate_ = trate; ech_ = tch;
      ComPtr<IMFMediaType> got;   // 以编码器实际版式为准（决定是否自组 ADTS）
      if (SUCCEEDED(mft_->GetOutputCurrentType(0, &got)) && got) {
        UINT32 q = 0, r2 = 0, c2 = 0;
        if (SUCCEEDED(got->GetUINT32(MF_MT_AAC_PAYLOAD_TYPE, &q))) pay_ = q;
        if (SUCCEEDED(got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r2)) && r2) erate_ = r2;
        if (SUCCEEDED(got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &c2)) && c2) ech_ = c2;
      }
      printf("[audio] AAC 协商 ok: %uHz/%uch payload=%s\n", erate_, ech_,
             pay_ == 1 ? "ADTS(直发)" : pay_ == 0 ? "raw(自组ADTS)" : "default");
    }
    if (!neg) { err_ = last; return false; }
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);   // 标准序：先 BEGIN 后 START_OF_STREAM
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
  }

  void EncodePcm(const int16_t* pcm, size_t frames, uint64_t ts_us) {
    const size_t bytes = frames * ech_ * 2;
    ComPtr<IMFSample> sp; ComPtr<IMFMediaBuffer> mb;
    if (FAILED(MFCreateSample(&sp)) || FAILED(MFCreateMemoryBuffer((DWORD)bytes, &mb))) return;
    BYTE* d = nullptr; if (FAILED(mb->Lock(&d, nullptr, nullptr))) return;
    memcpy(d, pcm, bytes); mb->Unlock();
    mb->SetCurrentLength((DWORD)bytes);
    sp->AddBuffer(mb.Get());
    LONGLONG t = (LONGLONG)ts_us * 10;              // µs→100ns
    sp->SetSampleTime(t); sp->SetSampleDuration((LONGLONG)frames * 10000000 / erate_);
    if (FAILED(mft_->ProcessInput(0, sp.Get(), 0))) return;
    for (int guard = 0; guard < 4; guard++) {
      ComPtr<IMFSample> os; ComPtr<IMFMediaBuffer> ob;
      MFCreateSample(&os); MFCreateMemoryBuffer(8192, &ob);
      ob->SetCurrentLength(8192);
      os->AddBuffer(ob.Get());
      MFT_OUTPUT_DATA_BUFFER od{}; od.pSample = os.Get();
      DWORD st = 0;
      HRESULT hr = mft_->ProcessOutput(0, 1, &od, &st);
      ComPtr<IMFSample> done; done.Attach(od.pSample); od.pSample = nullptr;
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT || FAILED(hr)) return;
      ComPtr<IMFMediaBuffer> b2;
      if (done && SUCCEEDED(done->GetBufferByIndex(0, &b2))) {
        BYTE* p = nullptr; DWORD len = 0;
        if (SUCCEEDED(b2->Lock(&p, nullptr, &len)) && len > 0 && len <= 7618) {
          if (pay_ == 1) {                 // 编码器已输出 ADTS：直发
            on_(p, len, ts_us);
          } else {                         // raw AAC：自组 7B ADTS 头（须在 Unlock 前取数）
            std::vector<uint8_t> f(7 + len);
            adts_header(f.data(), len, erate_, ech_);
            memcpy(f.data() + 7, p, len);
            b2->Unlock();
            on_(f.data(), f.size(), ts_us);
          }
        } else if (p) b2->Unlock();
      }
    }
  }

  // ---------- WASAPI loopback 采集 ----------
  void Run(int kbps) {
    HRESULT hr0 = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    OnScopeExit co{ [hr0] { if (SUCCEEDED(hr0)) CoUninitialize(); } };
    if (FAILED(hr0)) { err_ = hr0; return; }
    if (FAILED(MFStartup(MF_VERSION))) { err_ = E_FAIL; return; }
    if (!OpenEncoder(kbps)) { MFShutdown(); return; }

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
    if (FAILED(hr0)) { err_ = hr0; MFShutdown(); return; }

    if (FAILED(cli->Start())) { err_ = E_FAIL; MFShutdown(); return; }
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
      if (!nts || nts + 100000 < qnow || nts > qnow + 100000) nts = qnow;   // 与 QPC(=视频钟)偏差>100ms 时校正
      while (pcm.size() - carry >= 1024 * ech_) {
        EncodePcm(pcm.data() + carry, 1024, nts);
        nts += 1024ULL * 1000000 / erate_;
        carry += 1024 * ech_;
      }
      if (carry > 4096 * ech_) { pcm.erase(pcm.begin(), pcm.begin() + (long)carry); carry = 0; }
    }
    cli->Stop();
    MFShutdown();
  }

  struct OnScopeExit { std::function<void()> f; ~OnScopeExit() { f(); } };

  Fn on_;
  ComPtr<IMFTransform> mft_;
  DWORD pay_ = 0;
  UINT32 erate_ = 48000, ech_ = 2;   // 协商出的编码器版式
  std::thread thr_;
  std::atomic<bool> stop_{ false }, started_{ false };
  HRESULT err_ = S_OK;
};
} // namespace aud
