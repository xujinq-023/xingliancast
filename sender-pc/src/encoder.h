// H.264 MFT encoder wrapper: prefer hardware MFT, fall back to MS software. NV12 in, Annex-B AU out.
// NOTE: must be included AFTER mf.h in main.cpp (mftransform.h alone gives an incomplete IMFTransform).
#pragma once
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <functional>
#include <vector>
#include <cstring>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <wrl/client.h>
#include "logging.h"
using Microsoft::WRL::ComPtr;

class H264Encoder {
 public:
  using AuFn = std::function<void(const uint8_t* au, size_t n, bool key, bool idr)>;

 private:
  // 异步 MFT 事件回调（MF 线程池调用）：只数事件 + 重新武装，不碰 ProcessOutput（留主线程）。
  // 同步 GetEvent 对解锁后的 NVIDIA MFT 会死等（实测），必须用 BeginGetEvent 异步泵。
  struct EvCB : IMFAsyncCallback {
    explicit EvCB(H264Encoder* e) : e_(e) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID ri, void** pp) override {
      if (ri == __uuidof(IUnknown) || ri == __uuidof(IMFAsyncCallback)) { *pp = static_cast<IMFAsyncCallback*>(this); AddRef(); return S_OK; }
      *pp = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&rc_); }
    ULONG STDMETHODCALLTYPE Release() override {
      ULONG r = InterlockedDecrement(&rc_);
      if (!r) delete this;
      return r;
    }
    HRESULT STDMETHODCALLTYPE GetParameters(DWORD* f, DWORD* q) override {
      *f = 0; *q = MFASYNC_CALLBACK_QUEUE_MULTITHREADED; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult* r) override {
      ComPtr<IMFMediaEvent> ev;
      if (SUCCEEDED(e_->gen_->EndGetEvent(r, &ev)) && ev) {
        MediaEventType met;
        if (SUCCEEDED(ev->GetType(&met))) {
          if (met == METransformNeedInput || met == METransformHaveOutput) {
            std::lock_guard<std::mutex> lk(e_->mu_);
            if (met == METransformNeedInput) e_->need_++; else e_->have_++;
            e_->cv_.notify_all();
          }   // DrainComplete/Marker 等忽略
        }
      }
      if (e_->alive_) e_->gen_->BeginGetEvent(this, nullptr);   // 失败=已 Shutdown，静默退出
      return S_OK;
    }
    LONG rc_ = 1; H264Encoder* e_;
  };

 public:

  // 可选 D3D 纹理输入：mgr/token = 与 VP 纹理同一 D3D11 设备的 device manager。
  // 硬件 MFT 接受 SET_D3D_MANAGER 后 d3d()=true → NextInputTex/EncodeGPU；失败调用方回退 Encode()。
  bool Create(int w, int h, int fps, int kbps, bool prefer_hw,
              IMFDXGIDeviceManager* mgr = nullptr, UINT token = 0) {
    w_ = w; h_ = h; fps_ = fps; mgr_ = mgr; token_ = token;

    auto mkType = [&](GUID sub, bool out) {
      ComPtr<IMFMediaType> t; MFCreateMediaType(&t);
      t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      t->SetGUID(MF_MT_SUBTYPE, sub);
      MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
      MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, (UINT32)fps, 1);
      t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
      t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
      if (out) {
        t->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)kbps * 1000);
        // 硬件 MFT 拒绝缺 profile 的输出类型（实测 0xc00d36bd/36b4）；level 由 MFT 自行协商
        t->SetUINT32(MF_MT_VIDEO_PROFILE, 77);   // eAVEncH264VProfile_High
      } else t->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)w);
      return t;
    };
    // 属性→SetOutputType→[SET_D3D_MANAGER]→SetInputType→START/BEGIN（MS 编码器实测唯一可用顺序）
    // 异步 MFT（NVIDIA NVENC 等）：MF_TRANSFORM_ASYNC 属性解锁后走同样的同步调用，
    // 由 METransformNeedInput/HaveOutput 事件驱动；输入类型须取 GetInputAvailableType 原生 NV12。
    auto negotiate = [&](ComPtr<IMFTransform>& m, bool use_d3d, const char* tag) -> bool {
      ComPtr<IMFMediaType> it = mkType(MFVideoFormat_NV12, false), ot = mkType(MFVideoFormat_H264, true);
      ComPtr<IMFAttributes> at; m->GetAttributes(&at);
      UINT af = 0; if (at) at->GetUINT32(MF_TRANSFORM_ASYNC, &af);
      bool am = af != 0;
      if (at) {
        if (am && FAILED(at->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE)))
          return false;   // 解锁失败则无法按同步方式使用
        at->SetUINT32(MF_LOW_LATENCY, TRUE);
        at->SetUINT64(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        at->SetUINT64(CODECAPI_AVEncCommonMeanBitRate, (UINT64)kbps * 1000);
        at->SetUINT32(CODECAPI_AVEncCommonLowLatency, TRUE);
        at->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        at->SetUINT32(CODECAPI_AVEncMPVGOPSize, (UINT32)fps);   // 1s IDR = fast error recovery on lossy WiFi
        at->SetUINT32(CODECAPI_AVEncCommonRealTime, TRUE);
        // NVENC 质量/速度：0=最快, 100=质量最好。这里取 15 偏速度，1440p60 单帧 ~5-8ms。
        // （实测 85 会掉到 52ms/16fps，方向别搞反。）
        at->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed, 15);
      }
      HRESULT ho = m->SetOutputType(0, ot.Get(), 0);
      if (FAILED(ho)) { Logf("enc[%s] SetOutputType 0x%08lx\n", tag, ho); return false; }
      if (am) {
        // 异步：原生输入类型（内嵌 surface 描述）；SET_D3D 在此路径上会 AV，跳过——
        // MFCreateDXGISurfaceBuffer 自带设备信息，实测纹理输入可正常编码（probe/async.cpp）。
        ComPtr<IMFMediaType> pick;
        for (DWORD k = 0; k < 32; k++) {
          ComPtr<IMFMediaType> ti;
          if (FAILED(m->GetInputAvailableType(0, k, &ti))) break;
          GUID sub{}; ti->GetGUID(MF_MT_SUBTYPE, &sub);
          UINT32 tw = 0, th = 0; MFGetAttributeSize(ti.Get(), MF_MT_FRAME_SIZE, &tw, &th);
          if (sub == MFVideoFormat_NV12 && tw == (UINT32)w && th == (UINT32)h) { pick = ti; break; }
        }
        HRESULT hi = m->SetInputType(0, (pick ? pick : it).Get(), 0);
        if (FAILED(hi) && pick) hi = m->SetInputType(0, it.Get(), 0);
        if (FAILED(hi)) { Logf("enc[%s] async SetInputType 0x%08lx\n", tag, hi); return false; }
        m->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        ComPtr<IMFMediaEventGenerator> gen;
        if (FAILED(m.As(&gen)) || !gen) { Logf("enc[%s] async 无 eventgen\n", tag); return false; }
        mft_ = m; gen_ = gen; async_ = true; need_ = 0; have_ = 0;
        alive_ = true; d3d_ = use_d3d;
        EvCB* cb = new (std::nothrow) EvCB(this);
        if (!cb) return false;
        evcb_.Attach(cb);   // rc_=1 直接接管，不用 WRL Make（MSVC 对 lambda 内 Make<嵌套类> 解析报错）
        HRESULT hb = gen_->BeginGetEvent(evcb_.Get(), nullptr);
        if (FAILED(hb)) { Logf("enc[%s] async BeginGetEvent 0x%08lx\n", tag, hb); async_ = false; evcb_.Reset(); gen_.Reset(); return false; }
        return true;
      }
      bool d3d = false;
      if (use_d3d) {   // NVIDIA 官方 MFVideoEnc 样例顺序：输出类型后立刻注入 D3D manager
        ULONG_PTR p = (ULONG_PTR)&token_;   // MFT 在调用瞬间读取 token 数值
        HRESULT hm = m->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, p);
        if (SUCCEEDED(hm)) d3d = true;
        else Logf("enc[%s] SET_D3D 0x%08lx\n", tag, hm);
      }
      HRESULT hi = m->SetInputType(0, it.Get(), 0);
      if (FAILED(hi)) { Logf("enc[%s] SetInputType 0x%08lx\n", tag, hi); return false; }
      m->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
      m->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
      mft_ = m; d3d_ = d3d;
      return true;
    };
    auto try_enum = [&](DWORD flags, bool hwflag, bool use_d3d, const char* tag) -> bool {
      IMFActivate** acts = nullptr; UINT32 n = 0;
      // 实测：NV12→H264 触发类型过滤会把 NVIDIA H.264 MFT 筛掉(n=0)，无过滤枚举才有 12 个候选。
      // 类型兼容性交给 negotiate 的 SetInput/OutputType 逐个淘汰。
      HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, nullptr, &acts, &n);
      bool got = false;
      if (SUCCEEDED(hr) && acts) {
        for (UINT32 i = 0; i < n && !got; i++) {
          ComPtr<IMFTransform> t;
          HRESULT ha = acts[i]->ActivateObject(IID_PPV_ARGS(&t));
          if (FAILED(ha)) Logf("enc[%s] ActivateObject[%u] 0x%08lx\n", tag, i, ha);
          else if (negotiate(t, use_d3d, tag)) {
            got = true; hw_ = hwflag;
            WCHAR* nm = nullptr; UINT32 nl = 0; char who[192] = "?";
            if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nm, &nl)) && nm) {
              WideCharToMultiByte(CP_UTF8, 0, nm, -1, who, sizeof(who), nullptr, nullptr);
              CoTaskMemFree(nm);
            }
            Logf("enc[%s] 选中 #%u: %s (d3d=%d%s)\n", tag, i, who, d3d_ ? 1 : 0,
                   async_ ? " async" : "");
          }
        }
        for (UINT32 i = 0; i < n; i++) acts[i]->Release();
        CoTaskMemFree(acts);
      }
      if (!got) Logf("enc[%s]: 枚举 hr=0x%08lx 候选=%u\n", tag, hr, n);
      return got;
    };

    // 首选“解锁后的异步硬件 MFT”（NVIDIA NVENC 全功能版，probe 实测 5.6~11.7ms/帧）；
    // 其候选列表里也含同步 CIF 硬编，negotiate 按 MF_TRANSFORM_ASYNC 分流，天然互为回退。
    // 不带 SORTANDFILTER：保持枚举原始顺序（NVIDIA H.264 MFT 排第一），排序会改变/筛掉候选。
    if (prefer_hw) {
      if (mgr_)
        try_enum(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE, true, true, "async");
      if (!mft_ && mgr_)
        try_enum(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_HARDWARE, true, true, "hw+d3d");
      if (!mft_)
        try_enum(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_HARDWARE, true, false, "hw");
    }
    if (!mft_)
      try_enum(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, false, false, "sw");
    if (!mft_) {
      ComPtr<IMFTransform> m;
      HRESULT hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m));
      if (FAILED(hr) || !negotiate(m, false, "cmsh264")) { Logf("enc: 回退 CoCreate hr=0x%08lx\n", hr); return false; }
      hw_ = false;
    }
    return true;
  }

  // y/sy: Y plane + stride; uv/suv: NV12 chroma plane + stride. t100/dur100: 100ns units.
  bool Encode(const uint8_t* y, int sy, const uint8_t* uv, int suv,
              LONGLONG t100, LONGLONG dur100, bool force_idr, const AuFn& out,
              size_t yspan = SIZE_MAX) {
    if (async_ && !AwaitSlot(out)) return true;   // 流水线满：丢本帧（视作已处理，不计失败）
    ComPtr<IMFSample> sp; ComPtr<IMFMediaBuffer> mb;
    if (async_) {
      // 环复用：每帧新建 5.5MB 内存缓冲（+首次锁定时清零）在 60fps 下 = 330MB/s 的
      // 分配/清零，实测把编码耗时从 2.7ms 抬到 23~48ms。MFT 持槽到出帧，环 8 足够。
      int s = mri_; mri_ = (mri_ + 1) % 8;
      if (!msamp_[s]) {
        if (FAILED(MFCreateSample(&msamp_[s]))) return false;
        if (FAILED(MFCreateMemoryBuffer((DWORD)((size_t)w_ * h_ * 3 / 2), &mbuf_[s]))) return false;
        msamp_[s]->AddBuffer(mbuf_[s].Get());
      }
      sp = msamp_[s]; mb = mbuf_[s];
    } else {
      HRESULT h1 = MFCreateSample(&sp);
      HRESULT h2 = MFCreateMemoryBuffer((DWORD)((size_t)w_ * h_ * 3 / 2), &mb);
      if (FAILED(h1) || FAILED(h2)) return false;
      sp->AddBuffer(mb.Get());
    }
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
    sp->SetSampleTime(t100); sp->SetSampleDuration(dur100);
    if (force_idr) { sp->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
                     sp->SetUINT32(CODECAPI_AVEncVideoForceKeyFrame, TRUE); }
    HRESULT hrIn = mft_->ProcessInput(0, sp.Get(), 0);
    if (FAILED(hrIn)) return false;
    if (async_) { TookInput(); ConsumeOutputs(out); } else Drain(out);
    return true;
  }

  // D3D 纹理输入环：取下一槽 NV12 纹理（调用方把 VP 输出 CopyResource 进去），随后 EncodeGPU。
  // 环 8 帧：异步 MFT 在途可持有 4+ 帧输入，GPU 在途工作由 D3D11 引用计数保活。
  ID3D11Texture2D* NextInputTex(ID3D11Device* dev) {
    if (!d3d_ || !dev) return nullptr;
    if (!tex_[ri_]) {
      D3D11_TEXTURE2D_DESC d{};
      d.Width = w_; d.Height = h_; d.MipLevels = 1; d.ArraySize = 1;
      d.Format = DXGI_FORMAT_NV12; d.SampleDesc.Count = 1;
      d.Usage = D3D11_USAGE_DEFAULT;
      d.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;  // 视频引擎读写惯例
      if (FAILED(dev->CreateTexture2D(&d, nullptr, &tex_[ri_]))) return nullptr;
      HRESULT hx = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), tex_[ri_].Get(), 0, FALSE, &buf_[ri_]);
      if (FAILED(hx)) { Logf("enc: SurfaceBuffer hr=0x%08lx\n", hx); tex_[ri_].Reset(); return nullptr; }
    }
    cur_ = ri_; ri_ = (ri_ + 1) % 16;
    return tex_[cur_].Get();
  }
  // 输入 = NextInputTex 返回并已填好的纹理（MFCreateDXGISurfaceBuffer 包装，零拷贝交给 MFT）
  bool EncodeGPU(LONGLONG t100, LONGLONG dur100, bool force_idr, const AuFn& out) {
    if (!d3d_ || cur_ < 0 || !buf_[cur_]) return false;
    auto tsv = std::chrono::steady_clock::now();   // 临时诊断：慢帧分解
    if (async_ && !AwaitSlot(out)) return true;   // 流水线满：丢本帧
    auto tsi = std::chrono::steady_clock::now();
    ComPtr<IMFSample> sp;
    if (async_) {
      // 每槽复用 sample：输入 sample 被 MFT 持有到出帧，8 槽环 ≥ 在途深度，回收后再改时间戳
      if (!samp_[cur_]) {
        if (FAILED(MFCreateSample(&samp_[cur_])) ||
            FAILED(samp_[cur_]->AddBuffer(buf_[cur_].Get()))) return false;
      }
      sp = samp_[cur_];
    } else if (FAILED(MFCreateSample(&sp)) || FAILED(sp->AddBuffer(buf_[cur_].Get()))) return false;
    sp->SetSampleTime(t100); sp->SetSampleDuration(dur100);
    if (force_idr) { sp->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
                     sp->SetUINT32(CODECAPI_AVEncVideoForceKeyFrame, TRUE); }
    HRESULT hrIn = mft_->ProcessInput(0, sp.Get(), 0);
    auto tpi = std::chrono::steady_clock::now();
    if (FAILED(hrIn)) return false;
    if (async_) { TookInput(); ConsumeOutputs(out); } else Drain(out);
    auto toe = std::chrono::steady_clock::now();
    auto msl = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    double tot = msl(tsv, toe);
    if (tot > 30.0) {
      static std::chrono::steady_clock::time_point last{};
      if (msl(last, toe) > 900.0) {
        last = toe;
        Logf("enc[慢帧] %.1fms 等槽=%.1f 送帧=%.1f 收帧=%.1f\n",
               tot, msl(tsv, tsi), msl(tsi, tpi), msl(tpi, toe));
      }
    }
    return true;
  }

  // 输出抽取：同步 MFT 每次 ProcessOutput 至多一帧；所有权契约见注释
  void Drain(const AuFn& out) {
    for (int guard = 0; guard < 8; guard++) {
      // 预分配 output sample 并 Detach：所有权整体移交 ProcessOutput。
      // mfh264enc 在 NEED_MORE_INPUT 会释放 caller sample，ComPtr 双 Release
      // 导致 heap-UAF（ASan 实锤）；Detach 后只剩 MFT/caller 链一份引用。
      ComPtr<IMFSample> os; MFCreateSample(&os);
      ComPtr<IMFMediaBuffer> ob; MFCreateMemoryBuffer(1 << 23, &ob);  // 8MB：2K IDR 可达数 MB，过小 MS 编码器会直接 AV
      os->AddBuffer(ob.Get());
      MFT_OUTPUT_DATA_BUFFER od{}; od.dwStreamID = 0; od.pSample = os.Detach();
      DWORD st = 0;
      HRESULT hr = mft_->ProcessOutput(0, 1, &od, &st);
      // Contract: od.pSample 无论成败均归 caller；Attach 认领那一次引用。
      ComPtr<IMFSample> done; done.Attach(od.pSample); od.pSample = nullptr;
      if (od.pEvents) { od.pEvents->Release(); od.pEvents = nullptr; }
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
  }

  // ---- 异步 MFT 事件泵（解锁模式）：NeedInput 计可用输入槽，HaveOutput 收一帧 AU ----
  // 输出 sample 由 MFT 自己分配（pSample=NULL）。旧写法每次都预分配 8MB 再 Detach：
  // NVIDIA 异步 MFT 会返回它自己新建的 sample，被 Detach 的那块 8MB 从此无人释放 →
  // 每帧泄漏 8MB（实测 ~500MB/s，20 分钟涨到 22GB），系统提交内存被耗尽后
  // 连带 DSH 渲染进程/host 被 Windows OOM 杀掉。同步路径(Drain)不受影响。
  void DrainA(const AuFn& out) {
    for (int guard = 0; guard < 16; guard++) {
      MFT_OUTPUT_DATA_BUFFER od{}; od.dwStreamID = 0; od.pSample = nullptr;
      DWORD st = 0;
      HRESULT hr = mft_->ProcessOutput(0, 1, &od, &st);
      ComPtr<IMFSample> done; done.Attach(od.pSample); od.pSample = nullptr;
      if (od.pEvents) { od.pEvents->Release(); od.pEvents = nullptr; }
      if (FAILED(hr)) break;   // 事件与实态不符 / 需更多输入：下次再收
      ComPtr<IMFMediaBuffer> b2;
      if (done && FAILED(done->ConvertToContiguousBuffer(&b2))) done->GetBufferByIndex(0, &b2);
      if (b2) {
        BYTE* p = nullptr; DWORD len = 0;
        if (SUCCEEDED(b2->Lock(&p, nullptr, &len)) && len)
          acc_.insert(acc_.end(), p, p + len);
        b2->Unlock();
      }
      if (!acc_.empty()) {   // 一个 HaveOutput ≈ 一个完整 AU：聚合输出
        bool key = false, idr = false;
        for (size_t i = 0; i + 3 < acc_.size(); i++)
          if (acc_[i] == 0 && acc_[i+1] == 0 && acc_[i+2] == 1) {
            uint8_t t = acc_[i+3] & 0x1F;
            if (t == 5) idr = key = true; else if (t == 7) key = true;
          }
        out(acc_.data(), acc_.size(), key, idr);
        acc_.clear();
      }
    }
  }
  void ConsumeOutputs(const AuFn& out) {   // 主线程收 HaveOutput 计数对应的帧
    for (;;) {
      { std::lock_guard<std::mutex> lk(mu_); if (have_ <= 0) break; have_--; }
      DrainA(out);
    }
  }
  bool AwaitSlot(const AuFn& out) {  // 等一个输入槽，≤24ms，顺带收输出
    ConsumeOutputs(out);
    std::unique_lock<std::mutex> lk(mu_);
    for (int g = 0; g < 3 && need_ <= 0 && alive_; g++)
      cv_.wait_for(lk, std::chrono::milliseconds(8),
                   [&] { return need_ > 0 || !alive_; });
    return need_ > 0;
  }
  void TookInput() { std::lock_guard<std::mutex> lk(mu_); if (need_ > 0) need_--; }

  /// <summary>释放当前编码器（含异步事件泵），之后可以再次 Create() 换成别的实现。</summary>
  void ReleaseForRetry() {
    if (async_) {
      alive_ = false;
      ComPtr<IMFShutdown> sd;
      if (mft_ && SUCCEEDED(mft_.As(&sd)) && sd) sd->Shutdown();
      cv_.notify_all();
    } else if (mft_) {
      mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    }
    mft_.Reset(); gen_.Reset(); evcb_.Reset(); d3d_ = false;
    for (int i = 0; i < 16; i++) { tex_[i].Reset(); buf_[i].Reset(); samp_[i].Reset(); }
    for (int i = 0; i < 8; i++) { msamp_[i].Reset(); mbuf_[i].Reset(); }
    acc_.clear();
    async_ = false; hw_ = false; ri_ = 0; cur_ = -1; mri_ = 0; need_ = 0; have_ = 0;
  }

  bool hw() const { return hw_; }
  bool d3d() const { return d3d_; }
  bool async() const { return async_; }
  ~H264Encoder() {
    if (async_) {
      alive_ = false;
      ComPtr<IMFShutdown> sd;
      if (mft_ && SUCCEEDED(mft_.As(&sd)) && sd) sd->Shutdown();  // 同步取消挂起事件请求
      cv_.notify_all();
      evcb_.Reset(); gen_.Reset();
    } else if (mft_) mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
  }

 private:
  ComPtr<IMFTransform> mft_;
  std::vector<uint8_t> acc_;
  int w_ = 0, h_ = 0, fps_ = 60;
  bool hw_ = false;
  ComPtr<IMFDXGIDeviceManager> mgr_; UINT token_ = 0; bool d3d_ = false;
  ComPtr<ID3D11Texture2D> tex_[16]; ComPtr<IMFMediaBuffer> buf_[16];
  ComPtr<IMFSample> samp_[16]; int ri_ = 0, cur_ = -1;
  ComPtr<IMFSample> msamp_[8]; ComPtr<IMFMediaBuffer> mbuf_[8]; int mri_ = 0;   // mem 入参环
  ComPtr<IMFMediaEventGenerator> gen_; ComPtr<IMFAsyncCallback> evcb_;
  bool async_ = false;
  std::atomic<bool> alive_{false};
  std::mutex mu_; std::condition_variable cv_; int need_ = 0, have_ = 0;
};
