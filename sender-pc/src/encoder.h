// H.264 MFT encoder wrapper: prefer hardware MFT, fall back to MS software. NV12 in, Annex-B AU out.
// NOTE: must be included AFTER mf.h in main.cpp (mftransform.h alone gives an incomplete IMFTransform).
#pragma once
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <functional>
#include <vector>
#include <cstring>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

class H264Encoder {
 public:
  using AuFn = std::function<void(const uint8_t* au, size_t n, bool key, bool idr)>;

  bool Create(int w, int h, int fps, int kbps, bool prefer_hw) {
    w_ = w; h_ = h; fps_ = fps;
    MFT_REGISTER_TYPE_INFO ti{ MFVideoFormat_NV12, MFVideoFormat_H264 };

    auto mkType = [&](GUID sub, bool out) {
      ComPtr<IMFMediaType> t; MFCreateMediaType(&t);
      t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      t->SetGUID(MF_MT_SUBTYPE, sub);
      MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
      MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, (UINT32)fps, 1);
      t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
      t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
      if (out) t->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)kbps * 1000);
      else     t->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)w);
      return t;
    };
    // 属性→SetOutputType→SetInputType→START/BEGIN（MS 编码器实测唯一可用顺序）
    auto negotiate = [&](ComPtr<IMFTransform>& m) -> bool {
      ComPtr<IMFMediaType> it = mkType(MFVideoFormat_NV12, false), ot = mkType(MFVideoFormat_H264, true);
      ComPtr<IMFAttributes> at; m->GetAttributes(&at);
      if (at) {
        at->SetUINT32(MF_LOW_LATENCY, TRUE);
        at->SetUINT64(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        at->SetUINT64(CODECAPI_AVEncCommonMeanBitRate, (UINT64)kbps * 1000);
        at->SetUINT32(CODECAPI_AVEncCommonLowLatency, TRUE);
        at->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        at->SetUINT32(CODECAPI_AVEncMPVGOPSize, (UINT32)fps * 2);
        at->SetUINT32(CODECAPI_AVEncCommonRealTime, TRUE);
      }
      if (FAILED(m->SetOutputType(0, ot.Get(), 0))) return false;
      if (FAILED(m->SetInputType(0, it.Get(), 0))) return false;
      m->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
      m->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
      mft_ = m;
      return true;
    };
    auto try_enum = [&](DWORD flags, bool hwflag) -> bool {
      IMFActivate** acts = nullptr; UINT32 n = 0;
      HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &ti, &ti, &acts, &n);
      bool got = false;
      if (SUCCEEDED(hr) && acts) {
        for (UINT32 i = 0; i < n && !got; i++) {
          ComPtr<IMFTransform> t;
          if (SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&t))) && negotiate(t)) { got = true; hw_ = hwflag; }
        }
        for (UINT32 i = 0; i < n; i++) acts[i]->Release();
        CoTaskMemFree(acts);
      }
      return got;
    };

    // 先试可同步调用的硬编 MFT（NVENC/dxva 多注册为同步可用），失败回退软编；异步 NVENC 完整链路留 M5
    if (prefer_hw)
      try_enum(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, true);
    if (!mft_)
      try_enum(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, false);
    if (!mft_) {
      ComPtr<IMFTransform> m;
      HRESULT hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m));
      if (FAILED(hr) || !negotiate(m)) { printf("enc: 回退 CoCreate hr=0x%08lx\n", hr); return false; }
      hw_ = false;
    }
    return true;
  }

  // y/sy: Y plane + stride; uv/suv: NV12 chroma plane + stride. t100/dur100: 100ns units.
  bool Encode(const uint8_t* y, int sy, const uint8_t* uv, int suv,
              LONGLONG t100, LONGLONG dur100, bool force_idr, const AuFn& out,
              size_t yspan = SIZE_MAX) {
    ComPtr<IMFSample> sp; HRESULT h1 = MFCreateSample(&sp);
    ComPtr<IMFMediaBuffer> mb;
    HRESULT h2 = MFCreateMemoryBuffer((DWORD)((size_t)w_ * h_ * 3 / 2), &mb);
    if (FAILED(h1) || FAILED(h2)) return false;
    BYTE* d = nullptr;
    if (FAILED(mb->Lock(&d, nullptr, nullptr))) return false;
    for (int r = 0; r < h_; r++) {
      size_t off = (size_t)r * sy;
      size_t n = (off + (size_t)w_ <= yspan) ? (size_t)w_
                 : (off < yspan ? yspan - off : 0);
      if (n) memcpy(d + (size_t)r * w_, y + off, n);
      if (n < (size_t)w_) memset(d + (size_t)r * w_ + n, 0, (size_t)w_ - n);
    }
    for (int r = 0; r < h_ / 2; r++) memcpy(d + (size_t)w_ * h_ + (size_t)r * w_, uv + (size_t)r * suv, w_);
    mb->Unlock();
    sp->AddBuffer(mb.Get());
    sp->SetSampleTime(t100); sp->SetSampleDuration(dur100);
    if (force_idr) sp->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    HRESULT hrIn = mft_->ProcessInput(0, sp.Get(), 0);
    if (FAILED(hrIn)) return false;

    for (int guard = 0; guard < 8; guard++) {
      ComPtr<IMFSample> os; MFCreateSample(&os);
      ComPtr<IMFMediaBuffer> ob; MFCreateMemoryBuffer(1 << 23, &ob);  // 8MB：2K IDR 可达数 MB，过小 MS 编码器会直接 AV
      os->AddBuffer(ob.Get());
      MFT_OUTPUT_DATA_BUFFER od{}; od.dwStreamID = 0; od.pSample = os.Get();
      DWORD st = 0;
      HRESULT hr = mft_->ProcessOutput(0, 1, &od, &st);
      // Contract: output sample ownership transfers to caller; Attach takes that one release.
      ComPtr<IMFSample> done; done.Attach(od.pSample); od.pSample = nullptr;
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
      if (FAILED(hr)) break;
      ComPtr<IMFMediaBuffer> b2;
      if (done && SUCCEEDED(done->GetBufferByIndex(0, &b2))) {
        BYTE* p = nullptr; DWORD len = 0;
        if (SUCCEEDED(b2->Lock(&p, nullptr, &len))) {
          acc_.insert(acc_.end(), p, p + len);
          b2->Unlock();
        }
      }
      if (od.dwStatus & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE) continue; // 同步 MFT 理论上不会发生
      if (!acc_.empty()) {
        bool key = false, idr = false;
        for (size_t i = 0; i + 3 < acc_.size(); i++)
          if (acc_[i] == 0 && acc_[i+1] == 0 && acc_[i+2] == 1) {
            uint8_t t = acc_[i+3] & 0x1F;
            if (t == 5) idr = key = true; else if (t == 7) key = true;
          }
          out(acc_.data(), acc_.size(), key, idr);
        acc_.clear();
      }
      break;
    }
    return true;
  }

  bool hw() const { return hw_; }
  ~H264Encoder() {
    if (mft_) mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
  }

 private:
  ComPtr<IMFTransform> mft_;
  std::vector<uint8_t> acc_;
  int w_ = 0, h_ = 0, fps_ = 60;
  bool hw_ = false;
};
