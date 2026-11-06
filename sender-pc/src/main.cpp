// projector sender-pc (M2): 主显示器 WGC 采集 → D3D11 VP 转 NV12 → H.264 MFT → PJS/UDP → TV
// 用法: sender --ip 10.5.9.22 [--port 45690] [--bitrate 8000] [--fps 60] [--sw]
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.directx.direct3d11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include "pjs.h"
#include "encoder.h"

namespace caps = winrt::Windows::Graphics::Capture;
namespace gfx  = winrt::Windows::Graphics;
namespace gdx  = winrt::Windows::Graphics::DirectX;
namespace g11  = winrt::Windows::Graphics::DirectX::Direct3D11;

// 未公开的 GraphicsCaptureItem 工厂接口（对主显示器/窗口创建 item）
struct __declspec(uuid("c0a1d86e-813d-4f46-815d-22ff02056a6b"))
IGraphicsCaptureItemInterop : IUnknown {
  virtual HRESULT __stdcall CreateForMonitor(HMONITOR, REFIID, void**);
  virtual HRESULT __stdcall CreateForWindow(HWND, REFIID, void**);
};

static std::atomic<bool> g_stop{ false };
static BOOL WINAPI CtrlHandler(DWORD) { g_stop = true; return TRUE; }

struct Cfg { std::string ip = "10.5.9.22"; int port = pjs::DEFAULT_PORT; int bitrate = 8000; int fps = 60; bool hw = true; };

static Cfg ParseArgs(int argc, char** argv) {
  Cfg c;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i], v = argv[i + 1];
    if (k == "--ip") c.ip = v;
    else if (k == "--port") c.port = atoi(v.c_str());
    else if (k == "--bitrate") c.bitrate = atoi(v.c_str());
    else if (k == "--fps") c.fps = atoi(v.c_str());
  }
  for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--sw")) c.hw = false;
  return c;
}

int main(int argc, char** argv) {
  SetConsoleOutputCP(65001);
  Cfg cfg = ParseArgs(argc, argv);
  SetConsoleCtrlHandler(CtrlHandler, TRUE);

  pjs::UdpOut out;
  if (!out.open(cfg.ip.c_str(), cfg.port)) { printf("UDP 打开失败\n"); return 1; }
  printf("目标 %s:%d  码率 %dkbps  fps %d  偏好%s硬编\n", cfg.ip.c_str(), cfg.port,
         cfg.bitrate, cfg.fps, cfg.hw ? "" : "（禁）");

  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // ---------- D3D11 设备 ----------
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) { printf("D3D11 设备创建失败 0x%08lx\n", hr); return 2; }

    // ---------- 主显示器 → GraphicsCaptureItem ----------
    HMONITOR mon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    auto factory = winrt::get_activation_factory<caps::GraphicsCaptureItem>();
    auto interop = factory.as<IGraphicsCaptureItemInterop>();
    caps::GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(interop->CreateForMonitor(mon, winrt::guid_of<caps::GraphicsCaptureItem>(),
                                                   winrt::put_abi(item)));
    int w = (int)item.Size().Width, h = (int)item.Size().Height;
    printf("捕获 %dx%d\n", w, h);

    // ---------- 帧池 + 会话 ----------
    ComPtr<IDXGIDevice> dxgi; dev.As(&dxgi);
    g11::IDirect3DDevice d3ddev{ nullptr };
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(),
        reinterpret_cast<IInspectable**>(winrt::put_abi(d3ddev))));
    auto pool = caps::Direct3D11CaptureFramePool::CreateFreeThreaded(
        d3ddev, gdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, item.Size());
    auto session = pool.CreateCaptureSession(item);
    try { session.IsCursorCaptureEnabled(true); } catch (...) {}
    try { session.IsBorderRequired(false); } catch (...) {}

    // ---------- 视频处理器 BGRA→NV12 ----------
    ComPtr<ID3D11VideoDevice> vdev; dev.As(&vdev);
    ComPtr<ID3D11VideoContext> vctx; ctx->QueryInterface(IID_PPV_ARGS(&vctx));
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC vpd{};
    vpd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    vpd.InputWidth = w; vpd.InputHeight = h; vpd.OutputWidth = w; vpd.OutputHeight = h;
    vpd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    ComPtr<ID3D11VideoProcessorEnumerator> vpe;
    if (FAILED(vdev->CreateVideoProcessorEnumerator(&vpd, &vpe))) { printf("VP 枚举失败\n"); return 3; }
    ComPtr<ID3D11VideoProcessor> vp;
    winrt::check_hresult(vdev->CreateVideoProcessor(vpe.Get(), &vp));

    D3D11_TEXTURE2D_DESC nv12desc{};
    nv12desc.Width = w; nv12desc.Height = h; nv12desc.MipLevels = 1; nv12desc.ArraySize = 1;
    nv12desc.Format = DXGI_FORMAT_NV12; nv12desc.SampleDesc.Count = 1;
    nv12desc.Usage = D3D11_USAGE_DEFAULT; nv12desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> nv12tex; dev->CreateTexture2D(&nv12desc, nullptr, &nv12tex);
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovod{ D3D11_VPOV_DIMENSION_TEXTURE2D, { 0 } };
    ComPtr<ID3D11VideoProcessorOutputView> ov;
    winrt::check_hresult(vdev->CreateVideoProcessorOutputView(nv12tex.Get(), vpe.Get(), &ovod, &ov));

    D3D11_TEXTURE2D_DESC stdesc = nv12desc;
    stdesc.Usage = D3D11_USAGE_STAGING; stdesc.BindFlags = 0;
    stdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; dev->CreateTexture2D(&stdesc, nullptr, &staging);

    // ---------- 编码器 ----------
    H264Encoder enc;
    if (!enc.Create(w, h, cfg.fps, cfg.bitrate, cfg.hw)) { printf("H.264 编码器创建失败\n"); return 4; }
    printf("编码器: %s\n", enc.hw() ? "硬件(MFT)" : "软件(MS)");

    // ---------- 输出回调 ----------
    LARGE_INTEGER qpf{}; QueryPerformanceFrequency(&qpf);
    std::vector<uint8_t> cfg_sps;      // SPS/PPS（Annex-B）供 meta
    uint64_t frames = 0, t0 = GetTickCount64();
    auto on_au = [&](const uint8_t* au, size_t n, bool key, bool idr) {
      LARGE_INTEGER q; QueryPerformanceCounter(&q);
      uint64_t ts_us = (uint64_t)q.QuadPart * 1000000 / (uint64_t)qpf.QuadPart;
      if (cfg_sps.empty()) { auto c = pjs::extract_cfg(au, n); if (!c.empty()) cfg_sps = std::move(c); }
      out.send_au(au, n, ts_us, key, idr);
      frames++;
    };

    try { session.StartProjection(); } catch (...) {}
    printf("已推流: 按 Ctrl+C 退出\n");

    winrt::handle evt{ CreateEventW(nullptr, FALSE, FALSE, nullptr) };
    pool.FrameArrived([&](auto&&, auto&&) { SetEvent(evt.get()); });

    LONGLONG dur100 = 10000000 / cfg.fps;
    uint64_t last_meta = 0;
    bool first = true;

    while (!g_stop) {
      WaitForSingleObject(evt.get(), 250);
      while (auto frame = pool.TryGetNextFrame()) {
        auto surface = frame.Surface();
        ComPtr<IDirect3DDxgiInterfaceAccess> acc;
        if (SUCCEEDED(surface.as<IDirect3DDxgiInterfaceAccess>(&acc))) {
        ComPtr<ID3D11Texture2D> srcTex;
        if (SUCCEEDED(acc->GetInterface(IID_PPV_ARGS(&srcTex)))) {
          D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
          ivd.FourCC = 0; ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
          ComPtr<ID3D11VideoProcessorInputView> iv;
          if (SUCCEEDED(vdev->CreateVideoProcessorInputView(srcTex.Get(), vpe.Get(), &ivd, &iv))) {
            vctx->VideoProcessorSetStreamFrameFormat(vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
            vctx->VideoProcessorSetOutputColorSpace(vp.Get(), nullptr); // 保持默认 BT709/601 依驱动
            if (SUCCEEDED(vctx->VideoProcessorBlt(vp.Get(), ov.Get(), 0, 1, nullptr))) {
              ctx->CopyResource(staging.Get(), nv12tex.Get());
              D3D11_MAPPED_SUBRESOURCE mp{};
              if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mp))) {
                LONGLONG t100 = frame.Timestamp();
                if (t100 <= 0) { LARGE_INTEGER q; QueryPerformanceCounter(&q); t100 = q.QuadPart * 10000000 / qpf.QuadPart; }
                const uint8_t* yp = (const uint8_t*)mp.pData;
                const uint8_t* uv = yp + (size_t)mp.DepthPitch;
                enc.Encode(yp, (int)mp.RowPitch, uv, (int)mp.RowPitch, t100, dur100, first, on_au);
                ctx->Unmap(staging.Get(), 0);
                first = false;
              }
            }
          }
        }
        frame.Close();
      }
      uint64_t now = GetTickCount64();
      if (!cfg_sps.empty() && now - last_meta > 2000) {
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        uint64_t ts = (uint64_t)q.QuadPart * 1000000 / (uint64_t)qpf.QuadPart;
        out.send_meta(cfg_sps, ts);
        last_meta = now;
      }
      if (now - t0 > 1000) {
        double sec = (now - t0) / 1000.0;
        printf("fps=%.1f 包=%llu\n", (frames) / sec, (unsigned long long)out.packets());
        t0 = now; frames = 0;
      }
    }
    try { session.StopProjection(); } catch (...) {}
    printf("退出。共发 %llu 包\n", (unsigned long long)out.packets());
  } catch (winrt::hresult_error& e) {
    printf("WinRT 异常 0x%08lx: %ls\n", (unsigned long)e.code(), e.message().c_str());
    return 9;
  }
  return 0;
}
