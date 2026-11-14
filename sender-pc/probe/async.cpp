// 探针 v2：解锁后的“异步 MFT”= 属性解锁(MF_TRANSFORM_ASYNC_UNLOCK) + 同步 ProcessInput/Output + 事件驱动
// (METransformNeedInput / METransformHaveOutput，见 MS 文档 asynchronous-mfts)。
// 目的：验证 NVIDIA 硬件 MFT 以 D3D11 纹理 buffer 喂帧能否编出 H.264 AU 以及每帧耗时。
#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <cstdio>
#include <atomic>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

static const int NF = 30;  // 测 30 帧
static LARGE_INTEGER g_qpf;

struct Notify : IMFAsyncCallback {
  LONG refs = 1;
  IMFTransform* mft = nullptr;              // 不持引用
  ComPtr<IMFMediaEventGenerator> gen;       // MFT 的事件生成器
  ComPtr<IMFMediaBuffer> inbuf[4];
  ComPtr<IMFSample> insample[4];
  ComPtr<IMFMediaBuffer> outbuf;
  ComPtr<IMFSample> outsample;
  HANDLE done = nullptr;
  int sent = 0, got = 0;
  bool eos = false;
  std::atomic<HRESULT> fail{ 0 };
  LARGE_INTEGER t0;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override {
    if (r == IID_IUnknown || r == IID_IMFAsyncCallback) { *p = static_cast<IMFAsyncCallback*>(this); AddRef(); return S_OK; }
    *p = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG c = InterlockedDecrement(&refs);
    if (!c) delete this;
    return c;
  }
  HRESULT STDMETHODCALLTYPE GetParameters(DWORD* flags, DWORD* queue) override {
    *flags = 0; *queue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED; return S_OK;
  }
  void arm() {
    HRESULT hr = gen->BeginGetEvent(this, nullptr);
    if (FAILED(hr)) { printf("BeginGetEvent 0x%08lx\n", hr); fail = hr; SetEvent(done); }
  }
  HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult* r) override {
    ComPtr<IMFMediaEvent> ev;
    HRESULT hr = gen->EndGetEvent(r, &ev);
    if (FAILED(hr)) { printf("EndGetEvent 0x%08lx\n", hr); fail = hr; SetEvent(done); return S_OK; }
    MediaEventType met;
    ev->GetType(&met);
    if (met == METransformNeedInput) {
      if (sent < NF) {
        LONGLONG t100 = (LONGLONG)sent * 166667;  // 100ns 单位，60fps
        insample[sent % 4]->SetSampleTime(t100);
        insample[sent % 4]->SetSampleDuration(166667);
        hr = mft->ProcessInput(0, insample[sent % 4].Get(), 0);
        if (FAILED(hr)) { printf("ProcessInput[%d] 0x%08lx\n", sent, hr); fail = hr; SetEvent(done); return S_OK; }
        sent++;
      } else if (!eos) {
        eos = true;
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
      }
      arm();
    } else if (met == METransformHaveOutput) {
      MFT_OUTPUT_DATA_BUFFER odb = {};
      odb.dwStreamID = 0;
      odb.pSample = outsample.Get();
      DWORD stat = 0;
      hr = mft->ProcessOutput(0, 1, &odb, &stat);
      if (FAILED(hr)) { printf("ProcessOutput 0x%08lx\n", hr); fail = hr; SetEvent(done); return S_OK; }
      BYTE* p = nullptr; DWORD mx = 0, cl = 0;
      IMFSample* raw = odb.pSample ? odb.pSample : outsample.Get();
      ComPtr<IMFMediaBuffer> ob;
      if (SUCCEEDED(raw->ConvertToContiguousBuffer(&ob)) && ob && SUCCEEDED(ob->Lock(&p, &mx, &cl)) && p) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double ms = (now.QuadPart - t0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
        int nal = -1;
        for (DWORD k = 0; k + 4 < cl; k++)
          if (p[k] == 0 && p[k+1] == 0 && p[k+2] == 0 && p[k+3] == 1) { nal = p[k+4] & 0x1F; break; }
        got++;
        printf("frame %d: %u bytes  nal=%d  t=%.1fms\n", got, cl, nal, ms);
        ob->Unlock();
        if (ob.Get() == outbuf.Get()) ob->SetCurrentLength(0);
      } else {
        got++;
        printf("frame %d: (no buffer)\n", got);
      }
      if (raw == odb.pSample) raw->Release();
      if (got >= NF) { SetEvent(done); return S_OK; }
      arm();
    } else if (met == METransformDrainComplete) {
      printf("drain complete (got=%d sent=%d)\n", got, sent);
      if (got < NF) SetEvent(done); else arm();
    } else {
      arm();  // 其他事件（tick/streamtick）忽略继续泵
    }
    return S_OK;
  }
};

// SEH 包裹 ProcessMessage（async MFT 上 SET_D3D 可能 AV；helper 内无 C++ 对象避免 C2712）
static HRESULT SafeSetD3D(IMFTransform* t, UINT* token) {
  __try {
    return t->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)token);
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    return (HRESULT)0xC0000005;
  }
}

static char* w2a(const WCHAR* w, char* buf, int n) {
  buf[0] = 0; if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, n, nullptr, nullptr); return buf;
}

int main() {
  setbuf(stdout, nullptr);
  printf("start\n");
  QueryPerformanceFrequency(&g_qpf);
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) { printf("COM fail\n"); return 1; }
  if (FAILED(MFStartup(MF_VERSION))) { printf("MFStartup fail\n"); return 1; }

  ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> dctx;
  HRESULT hd = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &dev, nullptr, &dctx);
  if (FAILED(hd)) { printf("D3D11CreateDevice 0x%08lx\n", hd); return 1; }
  printf("d3d ok\n");

  ComPtr<IMFDXGIDeviceManager> mgr; UINT token = 0;
  MFCreateDXGIDeviceManager(&token, &mgr);
  mgr->ResetDevice(dev.Get(), token);

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = 2560; td.Height = 1440; td.MipLevels = 1; td.ArraySize = 1;
  td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
  ComPtr<IMFMediaBuffer> inb[4];
  for (int i = 0; i < 4; i++) {
    ComPtr<ID3D11Texture2D> t; dev->CreateTexture2D(&td, nullptr, &t);
    MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), t.Get(), 0, FALSE, &inb[i]);
  }
  printf("setup ok (d3d texture input)\n");

  ComPtr<IMFMediaType> ot; MFCreateMediaType(&ot);
  ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, 2560, 1440);
  MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, 60, 1);
  ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  ot->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  ot->SetUINT32(MF_MT_AVG_BITRATE, 8000000);
  ot->SetUINT32(MF_MT_VIDEO_PROFILE, 77);

  IMFActivate** acts = nullptr; UINT32 n = 0;
  HRESULT he = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                         MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE,
                         nullptr, nullptr, &acts, &n);
  printf("hw enum: hr=0x%08lx n=%u\n", he, n);
  if (FAILED(he) || !acts) return 1;

  for (UINT32 i = 0; i < n && i < 4; i++) {
    ComPtr<IMFTransform> t;
    if (FAILED(acts[i]->ActivateObject(IID_PPV_ARGS(&t)))) { printf("[%u] activate fail\n", i); continue; }
    char nm[192] = "?"; UINT32 asyncf = 0;
    {
      ComPtr<IMFAttributes> at0; WCHAR* p = nullptr; UINT32 nl = 0;
      t->GetAttributes(&at0);
      if (at0) {
        at0->GetUINT32(MF_TRANSFORM_ASYNC, &asyncf);
        if (SUCCEEDED(at0->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &p, &nl)) && p) {
          w2a(p, nm, sizeof(nm)); CoTaskMemFree(p);
        }
      }
    }
    printf("== try [%u] %s (async=%u)\n", i, nm, asyncf);

    // ---- 关键：解锁异步 MFT ----
    ComPtr<IMFAttributes> at; t->GetAttributes(&at);
    HRESULT hu = at->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    printf("  unlock 0x%08lx\n", hu);
    at->SetUINT32(MF_LOW_LATENCY, TRUE);
    at->SetUINT64(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    at->SetUINT64(CODECAPI_AVEncCommonMeanBitRate, 8000000);
    at->SetUINT32(CODECAPI_AVEncCommonLowLatency, TRUE);
    at->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    at->SetUINT32(CODECAPI_AVEncMPVGOPSize, 60);
    at->SetUINT32(CODECAPI_AVEncCommonRealTime, TRUE);
    printf("  codec attrs ok\n");

    HRESULT hm = 0;  // mem 输入：跳过 SET_D3D
    printf("  SET_D3D skipped(mem)\n");

    HRESULT h2 = t->SetOutputType(0, ot.Get(), 0);
    printf("  SetOutputType 0x%08lx\n", h2);
    if (FAILED(h2)) continue;
    // 输入类型从 MFT 提供的列表里挑 NV12（D3D 用法要求用原生类型）
    ComPtr<IMFMediaType> chosen;
    for (DWORD k = 0; k < 64; k++) {
      ComPtr<IMFMediaType> ti;
      if (FAILED(t->GetInputAvailableType(0, k, &ti))) break;
      GUID sub = {}; ti->GetGUID(MF_MT_SUBTYPE, &sub);
      UINT32 w = 0, hgt = 0; MFGetAttributeSize(ti.Get(), MF_MT_FRAME_SIZE, &w, &hgt);
      printf("  in[%u] sub={%08lx-%04x} %ux%u\n", k, sub.Data1, sub.Data2, w, hgt);
      if (sub == MFVideoFormat_NV12 && w == 2560 && hgt == 1440 && !chosen) chosen = ti;
    }
    if (!chosen) { printf("  no NV12 2560x1440 input type\n"); continue; }
    HRESULT h3 = t->SetInputType(0, chosen.Get(), 0);
    printf("  SetInputType(native) 0x%08lx\n", h3);
    if (FAILED(h3)) continue;

    t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    HRESULT h7 = t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    printf("  StartOfStream 0x%08lx\n", h7);
    if (FAILED(h7)) continue;

    ComPtr<IMFMediaEventGenerator> gen;
    HRESULT hg = t.As(&gen);
    printf("  QI(eventgen) 0x%08lx\n", hg);
    if (FAILED(hg)) continue;

    Notify* nfr = new Notify();
    ComPtr<Notify> nf; nf.Attach(nfr);
    nf->mft = t.Get(); nf->gen = gen;
    for (int k = 0; k < 4; k++) {
      nf->inbuf[k] = inb[k];
      MFCreateSample(&nf->insample[k]);
      nf->insample[k]->AddBuffer(inb[k].Get());
    }
    MFCreateMemoryBuffer(2 * 1024 * 1024, &nf->outbuf);
    MFCreateSample(&nf->outsample);
    nf->outsample->AddBuffer(nf->outbuf.Get());
    nf->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    QueryPerformanceCounter(&nf->t0);

    nf->arm();  // 之后全部由事件驱动
    DWORD w = WaitForSingleObject(nf->done, 20000);
    LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
    double el = (t1.QuadPart - nf->t0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
    printf("  wait=%s sent=%d got=%d fail=0x%08lx elapsed=%.1fms  avg=%.2fms/frame  ~fps=%.1f\n",
           w == WAIT_OBJECT_0 ? "ok" : (w == WAIT_TIMEOUT ? "TIMEOUT" : "err"),
           nf->sent, nf->got, nf->fail.load(), el,
           nf->got ? el / nf->got : 0.0, nf->got ? nf->got * 1000.0 / el : 0.0);

    ComPtr<IMFShutdown> sd;
    if (SUCCEEDED(t.As(&sd))) sd->Shutdown();

    for (UINT32 k = 0; k < n; k++) acts[k]->Release();
    CoTaskMemFree(acts);
    ExitProcess(nf->got > 0 ? 0 : 1);  // 跳过 COM 拆退（NVIDIA 拆退阶段会 AV）
  }
  for (UINT32 k = 0; k < n; k++) acts[k]->Release();
  CoTaskMemFree(acts);
  printf("no candidate negotiated\n");
  MFShutdown(); CoUninitialize();
  return 1;
}
