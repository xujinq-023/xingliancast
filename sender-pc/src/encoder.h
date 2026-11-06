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
    for (DWORD f : { (DWORD)MFT_ENUM_FLAG_HARDWARE, (DWORD)MFT_ENUM_FLAG_SOFTWARE }) {
      if (!prefer_hw && f == MFT_ENUM_FLAG_HARDWARE) continue;
      IMFActivate** acts = nullptr; UINT32 n = 0;
      HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
          MFT_ENUM_FLAG_SYNCMFT | f | MFT_ENUM_FLAG_SORTANDFILTER, &ti, &ti, &acts, &n);
      if (SUCCEEDED(hr) && acts) {
        for (UINT32 i = 0; i < n && !mft_; i++)
          if (SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&mft_)))) hw_ = (f == MFT_ENUM_FLAG_HARDWARE);
        for (UINT32 i = 0; i < n; i++) acts[i]->Release();
        CoTaskMemFree(acts);
      }
      if (mft_) break;
    }
    if (!mft_ && FAILED(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&mft_))))
      return false;

    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);

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
    ComPtr<IMFMediaType> it = mkType(MFVideoFormat_NV12, false), ot = mkType(MFVideoFormat_H264, true);
    if (FAILED(mft_->SetInputStreamType(0, it.Get())))  return false;
    if (FAILED(mft_->SetOutputStreamType(0, ot.Get()))) return false;

    ComPtr<IMFAttributes> at; mft_->GetAttributes(&at);
    if (at) {
      at->SetUINT32(MF_LOW_LATENCY, TRUE);
      at->SetUINT64(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
      at->SetUINT64(CODECAPI_AVEncCommonMeanBitRate, (UINT64)kbps * 1000);
      at->SetUINT32(CODECAPI_AVEncCommonLowLatency, TRUE);
      at->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
      at->SetUINT32(CODECAPI_AVEncMPVGOPSize, (UINT32)fps * 2);
      at->SetUINT32(CODECAPI_AVEncCommonRealTime, TRUE);
    }
    return true;
  }

  // y/sy: Y plane + stride; uv/suv: NV12 chroma plane + stride. t100/dur100: 100ns units.
  bool Encode(const uint8_t* y, int sy, const uint8_t* uv, int suv,
              LONGLONG t100, LONGLONG dur100, bool force_idr, const AuFn& out) {
    ComPtr<IMFSample> sp; MFCreateSample(&sp);
    ComPtr<IMFMediaBuffer> mb;
    MFCreateMemoryBuffer((DWORD)((size_t)w_ * h_ * 3 / 2), &mb);
    BYTE* d = nullptr;
    if (FAILED(mb->Lock(&d, nullptr, nullptr))) return false;
    for (int r = 0; r < h_; r++)     memcpy(d + (size_t)r * w_, y + (size_t)r * sy, w_);
    for (int r = 0; r < h_ / 2; r++) memcpy(d + (size_t)w_ * h_ + (size_t)r * w_, uv + (size_t)r * suv, w_);
    mb->Unlock();
    sp->SetBuffer(mb.Get());
    sp->SetSampleTime(t100); sp->SetSampleDuration(dur100);
    if (force_idr) sp->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    if (FAILED(mft_->ProcessInput(0, sp.Get(), 0))) return false;

    for (int guard = 0; guard < 8; guard++) {
      ComPtr<IMFSample> os; MFCreateSample(&os);
      ComPtr<IMFMediaBuffer> ob; MFCreateMemoryBuffer(1 << 20, &ob);
      os->SetBuffer(ob.Get());
      MFT_OUTPUT_DATA_BUFFER od{}; od.dwStreamID = 0; od.pSample = os.Get();
      DWORD st = 0;
      HRESULT hr = mft_->ProcessOutput(0, 1, &od, &st);
      // Contract: output sample ownership transfers to caller; Attach takes that one release.
      ComPtr<IMFSample> done; done.Attach(od.pSample, false); od.pSample = nullptr;
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
      if (FAILED(hr) && hr != MF_E_INCOMPLETE_SAMPLE) break;
      bool incomplete = (hr == MF_E_INCOMPLETE_SAMPLE);
      ComPtr<IMFMediaBuffer> b2;
      if (done && SUCCEEDED(done->GetBufferByIndex(0, &b2))) {
        BYTE* p = nullptr; DWORD len = 0;
        if (SUCCEEDED(b2->Lock(&p, nullptr, &len))) {
          acc_.insert(acc_.end(), p, p + len);
          b2->Unlock();
        }
      }
      if (done && SUCCEEDED(done->GetUINT32(MFSampleExtension_Discontinuity, &st)) && (st & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE))
        incomplete = true;
      if (od.dwStatus & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE) incomplete = true;
      if (incomplete) continue;
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
    if (mft_) {
      mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_DRAIN_STREAM, 0);
      mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    }
  }

 private:
  ComPtr<IMFTransform> mft_;
  std::vector<uint8_t> acc_;
  int w_ = 0, h_ = 0, fps_ = 60;
  bool hw_ = false;
};
