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
#include <Windows.Graphics.Capture.Interop.h>
#include "pjs.h"
#include "encoder.h"
#include "audio.h"

namespace caps = winrt::Windows::Graphics::Capture;
namespace gfx  = winrt::Windows::Graphics;
namespace gdx  = winrt::Windows::Graphics::DirectX;
namespace g11  = winrt::Windows::Graphics::DirectX::Direct3D11;
namespace dx11i = Windows::Graphics::DirectX::Direct3D11;   // interop 头（非 winrt:: 下）

static std::atomic<bool> g_stop{ false };
static BOOL WINAPI CtrlHandler(DWORD) { g_stop = true; return TRUE; }

struct Cfg { std::string ip = "10.5.9.22"; int port = pjs::DEFAULT_PORT; int bitrate = 8000; int fps = 60; bool hw = true; int height = 0; bool audio = true; };

static Cfg ParseArgs(int argc, char** argv) {
  Cfg c;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i], v = argv[i + 1];
    if (k == "--ip") c.ip = v;
    else if (k == "--port") c.port = atoi(v.c_str());
    else if (k == "--bitrate") c.bitrate = atoi(v.c_str());
    else if (k == "--fps") c.fps = atoi(v.c_str());
    else if (k == "--height") c.height = atoi(v.c_str());
  }
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--sw")) c.hw = false;
    if (!strcmp(argv[i], "--noaudio")) c.audio = false;
  }
  return c;
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  SetConsoleOutputCP(65001);
  Cfg cfg = ParseArgs(argc, argv);
  SetConsoleCtrlHandler(CtrlHandler, TRUE);

  pjs::UdpOut out;
  if (!out.open(cfg.ip.c_str(), cfg.port)) { printf("UDP 打开失败\n"); return 1; }
  printf("目标 %s:%d  码率 %dkbps  fps %d  偏好%s硬编\n", cfg.ip.c_str(), cfg.port,
         cfg.bitrate, cfg.fps, cfg.hw ? "" : "（禁）");
  MFStartup(MF_VERSION);

  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // ---------- D3D11 设备 ----------
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl;
    // 硬件设备（实测吞吐由屏幕内容变化率决定，非管线瓶颈；WARP 无优势）
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
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
    // 编码分辨率：--height N 时整数倍降采样（CPU 转换循环步进采样）
    int s = 1;
    if (cfg.height > 0 && cfg.height * 2 <= h) s = h / cfg.height;
    if (s < 1) s = 1; if (s > 8) s = 8;
    int ew = (w / s) & ~1, eh = (h / s) & ~1;
    printf("捕获 %dx%d  编码 %dx%d (1/%d)\n", w, h, ew, eh, s);

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

    // ---------- staging（BGRA → CPU 转 NV12；GPU VP 路径 M5 再建） ----------
    D3D11_TEXTURE2D_DESC nv12desc{};
    nv12desc.Width = w; nv12desc.Height = h; nv12desc.MipLevels = 1; nv12desc.ArraySize = 1;
    nv12desc.Format = DXGI_FORMAT_NV12; nv12desc.SampleDesc.Count = 1;
    nv12desc.Usage = D3D11_USAGE_DEFAULT;
    nv12desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_TEXTURE2D_DESC stdesc = nv12desc;
    stdesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // staging 用 BGRA（Map 语义无歧义），NV12 转换走 CPU
    stdesc.Usage = D3D11_USAGE_STAGING; stdesc.BindFlags = 0;
    stdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr2 = dev->CreateTexture2D(&stdesc, nullptr, &staging);
    if (FAILED(hr2)) { fprintf(stderr, "staging hr=0x%08lx\n", hr2); return 3; }
    std::vector<uint8_t> yuv((size_t)ew * eh * 3 / 2);  // CPU 转出的 packed NV12（行距=ew）

    // ---------- 编码器 ----------
    H264Encoder enc;
    if (!enc.Create(ew, eh, cfg.fps, cfg.bitrate, cfg.hw)) { printf("H.264 编码器创建失败\n"); return 4; }
    printf("编码器: %s\n", enc.hw() ? "硬件(MFT)" : "软件(MS)");

    // ---------- M3 音频: WASAPI loopback → AAC(128k) → PJS ----------
    aud::AudioPipe apipe;
    if (cfg.audio) {
      apipe.Start(128, [&](const uint8_t* a, size_t n, uint64_t ts) { out.send_audio(a, n, ts); });
      Sleep(400);
      if (apipe.started()) printf("音频: loopback+AAC-128k\n");
      else if (apipe.err() == S_OK) printf("音频: 启动中\n");
      else printf("音频: 不可用(仅视频) hr=0x%08X\n", (unsigned)apipe.err());
    }

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

    try { session.StartCapture(); } catch (...) {}
    printf("已推流: 按 Ctrl+C 退出\n");

    winrt::handle evt{ CreateEventW(nullptr, FALSE, FALSE, nullptr) };
    pool.FrameArrived([&](auto&&, auto&&) { SetEvent(evt.get()); });

    LONGLONG dur100 = 10000000 / cfg.fps;
    const uint64_t frame_int_us = 1000000 / (uint64_t)cfg.fps;  // 限帧间隔（QPC 微秒，tick 分辨率 15.6ms 不够）
    uint64_t last_enc = 0, last_meta = 0, conv_us = 0, enc_us = 0;
    bool first = true;

    while (!g_stop) {
      WaitForSingleObject(evt.get(), 250);
      while (auto frame = pool.TryGetNextFrame()) {
        auto surface = frame.Surface();
        dx11i::IDirect3DDxgiInterfaceAccess* rawacc = nullptr;
        if (SUCCEEDED(surface.as(__uuidof(dx11i::IDirect3DDxgiInterfaceAccess), (void**)&rawacc))) {
        ComPtr<dx11i::IDirect3DDxgiInterfaceAccess> acc; acc.Attach(rawacc);
        ComPtr<ID3D11Texture2D> srcTex;
        if (SUCCEEDED(acc->GetInterface(IID_PPV_ARGS(&srcTex)))) {
          LARGE_INTEGER qi; QueryPerformanceCounter(&qi);
          uint64_t nowus = (uint64_t)qi.QuadPart * 1000000 / (uint64_t)qpf.QuadPart;
          if (first || nowus - last_enc + 1 >= frame_int_us) {   // 限帧：跳过多余刷新帧
            ctx->CopyResource(staging.Get(), srcTex.Get());   // 同格式 BGRA 拷贝
          D3D11_MAPPED_SUBRESOURCE mp{};
          if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mp))) {
            // BGRA → packed NV12（BT.601 limited range），行距=ew，1/s 步进采样缩放
            LARGE_INTEGER c0, c1, c2; QueryPerformanceCounter(&c0);
            const uint8_t* bgra = (const uint8_t*)mp.pData;
            const int sp = (int)mp.RowPitch;
            uint8_t* Yp = yuv.data(); uint8_t* UVp = Yp + (size_t)ew * eh;
            for (int j2 = 0; j2 < eh; j2 += 2) {
              int j = j2 * s;
              const uint8_t* r0 = bgra + (size_t)j * sp;
              const uint8_t* r1 = bgra + (size_t)(j + s < h ? j + s : h - 1) * sp;
              uint8_t* y0 = Yp + (size_t)j2 * ew; uint8_t* y1 = y0 + ew;
              uint8_t* uv = UVp + (size_t)(j2 / 2) * ew;
              for (int i2 = 0; i2 < ew; i2++) {
                const uint8_t* p0 = r0 + (i2 * s) * 4; const uint8_t* p1 = r1 + (i2 * s) * 4;
                y0[i2] = (uint8_t)((((int)(66 * p0[2] + 129 * p0[1] + 25 * p0[0] + 128)) >> 8) + 16);
                y1[i2] = (uint8_t)((((int)(66 * p1[2] + 129 * p1[1] + 25 * p1[0] + 128)) >> 8) + 16);
                if ((i2 & 1) == 0 && i2 + 1 < ew) {
                  const uint8_t* q0 = p0 + s * 4; const uint8_t* q1 = p1 + s * 4;
                  int B = (p0[0] + q0[0] + p1[0] + q1[0]) >> 2;
                  int G = (p0[1] + q0[1] + p1[1] + q1[1]) >> 2;
                  int R = (p0[2] + q0[2] + p1[2] + q1[2]) >> 2;
                  uv[i2]     = (uint8_t)((((int)(-38 * R - 74 * G + 112 * B + 128)) >> 8) + 128);
                  uv[i2 + 1] = (uint8_t)((((int)(94 * R - 88 * G - 6 * B + 128)) >> 8) + 128);
                }
              }
            }
            ctx->Unmap(staging.Get(), 0);
            QueryPerformanceCounter(&c1);
            LONGLONG t100 = (LONGLONG)frame.SystemRelativeTime().count();
            if (t100 <= 0) { LARGE_INTEGER q; QueryPerformanceCounter(&q); t100 = q.QuadPart * 10000000 / qpf.QuadPart; }
            enc.Encode(Yp, ew, UVp, ew, t100, dur100, first, on_au);
            QueryPerformanceCounter(&c2);
            conv_us += (uint64_t)(c1.QuadPart - c0.QuadPart) * 1000000 / (uint64_t)qpf.QuadPart;
            enc_us  += (uint64_t)(c2.QuadPart - c1.QuadPart) * 1000000 / (uint64_t)qpf.QuadPart;
            first = false;
            LARGE_INTEGER qe; QueryPerformanceCounter(&qe);
            last_enc = (uint64_t)qe.QuadPart * 1000000 / (uint64_t)qpf.QuadPart;
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
        printf("fps=%.1f 包=%llu 音包=%u 转换=%.1fms 编码=%.1fms\n", frames / sec,
               (unsigned long long)out.packets(), (unsigned)out.audio_packets(),
               frames ? conv_us / frames / 1000.0 : 0.0,
               frames ? enc_us / frames / 1000.0 : 0.0);
        t0 = now; frames = 0; conv_us = enc_us = 0;
      }
    }
    try { session.Close(); } catch (...) {}
    printf("退出。共发 %llu 包\n", (unsigned long long)out.packets());
  } catch (winrt::hresult_error& e) {
    printf("WinRT 异常 0x%08lx: %ls\n", (unsigned long)e.code(), e.message().c_str());
    return 9;
  }
  return 0;
}
