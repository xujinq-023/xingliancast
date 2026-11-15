// projector sender-pc (M2): 主显示器 WGC 采集 → D3D11 VP 转 NV12 → H.264 MFT → PJS/UDP → TV
// 用法: sender --ip 10.5.9.22 [--port 45690] [--bitrate 8000] [--fps 60] [--display N] [--height N] [--sw] [--noaudio]
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>   // IDXGIOutput1::DuplicateOutput（WGC 被拒时的回退捕获）
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <algorithm>
#include <deque>
#include <mutex>
#include <thread>
#include <chrono>
#include <winrt/base.h>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.directx.direct3d11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <Windows.Graphics.Capture.Interop.h>
#include "logging.h"
#include "pjs.h"
#include "license.h"
#include "discover.h"
#include "feedback.h"
#include "encoder.h"
#include "audio.h"

namespace caps = winrt::Windows::Graphics::Capture;
namespace gfx  = winrt::Windows::Graphics;
namespace gdx  = winrt::Windows::Graphics::DirectX;
namespace g11  = winrt::Windows::Graphics::DirectX::Direct3D11;
namespace dx11i = Windows::Graphics::DirectX::Direct3D11;   // interop 头（非 winrt:: 下）

// 在 NV12 平面上做一次可分离盒式模糊（就地为隐私打码，锁屏暂停时用）
static void BlurPlane(uint8_t* p, int w, int h, int radius) {
  if (!p || w <= 0 || h <= 0 || radius <= 0) return;
  std::vector<uint8_t> tmp((size_t)w * h);
  // 水平
  for (int y = 0; y < h; y++) {
    uint8_t* row = p + (size_t)y * w;
    int acc = 0, cnt = 0;
    for (int x = 0; x < w + radius; x++) {
      if (x < w) { acc += row[x]; cnt++; }
      if (x - radius - 1 >= 0) { acc -= row[x - radius - 1]; cnt--; }
      int dx = x - radius;
      if (dx >= 0 && dx < w) tmp[(size_t)y * w + dx] = (uint8_t)(acc / (cnt ? cnt : 1));
    }
  }
  // 垂直
  for (int x = 0; x < w; x++) {
    int acc = 0, cnt = 0;
    for (int y = 0; y < h + radius; y++) {
      if (y < h) { acc += tmp[(size_t)y * w + x]; cnt++; }
      if (y - radius - 1 >= 0) { acc -= tmp[(size_t)(y - radius - 1) * w + x]; cnt--; }
      int dy = y - radius;
      if (dy >= 0 && dy < h) p[(size_t)dy * w + x] = (uint8_t)(acc / (cnt ? cnt : 1));
    }
  }
}

static std::atomic<bool> g_stop{ false };
// 锁屏/安全桌面检测：输入桌面打不开（Winlogon 正在占用）即视为锁屏。
// 不能只依赖 DXGI 的 ACCESS_LOST——实测部分驱动锁屏时只返回超时，永远不报错，
// 表现就是"画面冻住但连接没断"。
static std::atomic<bool> g_paused{ false };   // 锁屏期间暂停抓屏（仍保持心跳）
static bool InputDesktopLocked() {
  HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP);
  if (!d) return true;
  CloseDesktop(d);
  return false;
}
static bool g_noenc = false;    // 诊断：PJS_NOENC=1 跳过编码（保留采集+转换+回读）
static bool g_nosend = false;   // 诊断：PJS_NOSEND=1 不发视频包
static bool g_slowlog = false;  // 诊断：PJS_SLOWLOG=1 每 10 秒才打一行统计
static bool g_curdbg = false;   // 诊断：PJS_CURSORDBG=1 每秒打印光标状态
static BOOL WINAPI CtrlHandler(DWORD) { g_stop = true; return TRUE; }

// 日志统一走 logging.h（后台线程写盘 + 按大小轮换 + 崩溃记录）。
using logx::Logf; using logx::Warnf; using logx::Errf;

// ---------- 鼠标指针叠加（DDA 的帧不含硬件光标，必须自己画） ----------
struct CursorState {
  std::vector<uint8_t> shape;          // 最近一次 PointerShapeBuffer（MONO 时 AND 掩码 + XOR 掩码）
  UINT type = 0, w = 0, h = 0, pitch = 0, hotX = 0, hotY = 0;
  bool visible = false;
  int px = 0, py = 0;                  // 相对被捕获输出左上角
};
static CursorState g_cur;


// ---------- 光标合成 v2：交给 DrawIconEx 解析，避免自己处理 COLOR/MASKED/MONO 掩码 ----------
struct CurBitmap {
  HBITMAP bmp = nullptr;
  HDC dc = nullptr;
  uint32_t* bits = nullptr;
  int size = 0;          // 边长（像素）
  bool Ensure(int side) {
    if (bmp && size >= side) return true;
    if (bmp) { DeleteObject(bmp); bmp = nullptr; }
    if (dc) { DeleteDC(dc); dc = nullptr; }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = side;
    bi.bmiHeader.biHeight = -side;        // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* p = nullptr;
    dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    if (!bmp || !p) return false;
    SelectObject(dc, bmp);
    bits = (uint32_t*)p;
    size = side;
    return true;
  }
};

static CurBitmap g_curBmpB;   // 画在黑底上
/// 取当前光标像素。用「黑底/白底各画一次」还原 alpha，天然兼容 COLOR/MASKED/MONO 三种光标。
static bool GrabCursorImage(CurBitmap& cb, CurBitmap& cbB, int& w, int& h, int& hotX, int& hotY) {
  CURSORINFO ci{};
  ci.cbSize = sizeof(ci);
  if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return false;
  ICONINFO ii{};
  if (!GetIconInfo(ci.hCursor, &ii)) return false;
  BITMAP bm{};
  int side = 64;
  if (ii.hbmColor && GetObject(ii.hbmColor, sizeof(bm), &bm)) {
    w = bm.bmWidth; h = bm.bmHeight;
  } else if (ii.hbmMask && GetObject(ii.hbmMask, sizeof(bm), &bm)) {
    w = bm.bmWidth; h = bm.bmHeight / 2;
  } else {
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return false;
  }
  hotX = (int)ii.xHotspot; hotY = (int)ii.yHotspot;
  if (ii.hbmColor) DeleteObject(ii.hbmColor);
  if (ii.hbmMask) DeleteObject(ii.hbmMask);
  if (w <= 0 || h <= 0 || w > 256 || h > 256) return false;
  side = ((w > h ? w : h) + 3) & ~3;
  if (!cb.Ensure(side) || !cbB.Ensure(side)) return false;

  // 白底 / 黑底各画一次
  memset(cb.bits, 0xFF, (size_t)cb.size * cb.size * 4);    // 白 (BGRA 全 FF)
  memset(cbB.bits, 0x00, (size_t)cbB.size * cbB.size * 4); // 黑
  DrawIconEx(cb.dc, 0, 0, ci.hCursor, w, h, 0, nullptr, DI_NORMAL);
  DrawIconEx(cbB.dc, 0, 0, ci.hCursor, w, h, 0, nullptr, DI_NORMAL);

  // alpha = 255 - (白底像素 - 黑底像素)；颜色按黑底反预乘还原
  for (int i = 0, n = cb.size * cb.size; i < n; i++) {
    const uint32_t pw = cb.bits[i], pb = cbB.bits[i];
    const int bw = pw & 0xFF, gw = (pw >> 8) & 0xFF, rw = (pw >> 16) & 0xFF;
    const int bb = pb & 0xFF, gb = (pb >> 8) & 0xFF, rb = (pb >> 16) & 0xFF;
    const int delta = ((rw - rb) + (gw - gb) + (bw - bb)) / 3;   // 0..255
    int a = 255 - delta;
    if (a < 8) { cb.bits[i] = 0; continue; }
    if (a > 255) a = 255;
    const int r = rb * 255 / a, g = gb * 255 / a, b = bb * 255 / a;
    cb.bits[i] = ((uint32_t)a << 24) | ((uint32_t)(r > 255 ? 255 : r) << 16) |
                 ((uint32_t)(g > 255 ? 255 : g) << 8) | (uint32_t)(b > 255 ? 255 : b);
  }
  return true;
}
/// 把 BGRA（直通 alpha）光标合成到 NV12 的 Y 平面，并更新对应 2x2 块的色度。
static void BlendCursorImage(uint8_t* base, int pitch, int W, int H,
                             const CurBitmap& cb, int px, int py, int hotX, int hotY, int w, int h) {
  const int x0 = px - hotX, y0 = py - hotY;
  uint8_t* uvBase = base + (size_t)H * pitch;
  for (int row = 0; row < h; row++) {
    const int y = y0 + row;
    if (y < 0 || y >= H) continue;
    const uint32_t* srow = cb.bits + (size_t)row * cb.size;
    uint8_t* drow = base + (size_t)y * pitch;
    for (int col = 0; col < w; col++) {
      const int x = x0 + col;
      if (x < 0 || x >= W) continue;
      const uint32_t p = srow[col];
      const int a = (p >> 24) & 0xFF;
      if (!a) continue;                                   // 透明
      const int B = p & 0xFF, G = (p >> 8) & 0xFF, R = (p >> 16) & 0xFF;
      const int Y = ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16;
      uint8_t* d = drow + x;
      *d = (uint8_t)(a >= 250 ? Y : (*d * (255 - a) + Y * a) / 255);
      if (a >= 128) {
        const int U = ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
        const int V = ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;
        uint8_t* uv = uvBase + (size_t)(y >> 1) * pitch + (x & ~1);
        uv[0] = (uint8_t)U; uv[1] = (uint8_t)V;
      }
    }
  }
}
static CurBitmap g_curBmp;

struct Cfg { std::string ip = ""; int port = pjs::DEFAULT_PORT; int bitrate = 8000; int fps = 60; bool hw = true; int height = 0; bool audio = true; int display = 0; std::string logPath; int logSizeMB = 5; bool switched = false; };

static Cfg ParseArgs(int argc, char** argv) {
  Cfg c;
  // 逐个 token 解析：不能按「成对」读参数——`--noaudio`/`--sw` 是纯开关，
  // 按对读会把后面的 `--ip` 当成它的值吃掉，导致界面传的 IP 丢失、只能走自动发现而失败。
  for (int i = 1; i < argc; i++) {
    std::string k = argv[i];
    auto val = [&]() -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
    if (k == "--ip") c.ip = val();
    else if (k == "--port") { c.port = atoi(val().c_str()); if (c.port < 1 || c.port > 65535) c.port = pjs::DEFAULT_PORT; }
    else if (k == "--bitrate") { c.bitrate = atoi(val().c_str()); if (c.bitrate < 100) c.bitrate = 100; if (c.bitrate > 100000) c.bitrate = 100000; }
    else if (k == "--fps") { c.fps = atoi(val().c_str()); if (c.fps < 1) c.fps = 1; if (c.fps > 240) c.fps = 240; }
    else if (k == "--height") { c.height = atoi(val().c_str()); if (c.height < 0) c.height = 0; if (c.height > 4320) c.height = 4320; }
    else if (k == "--display") { c.display = atoi(val().c_str()); if (c.display < 0) c.display = 0; if (c.display > 63) c.display = 63; }
    else if (k == "--log") c.logPath = val();
    else if (k == "--log-size") { int mb = atoi(val().c_str()); c.logSizeMB = mb < 5 ? 5 : (mb > 100 ? 100 : mb); }
    else if (k == "--sw") c.hw = false;
    else if (k == "--switched") c.switched = true;   // 本次启动是「被投显示器消失→已切主屏」
    else if (k == "--noaudio") c.audio = false;
  }
  return c;
}

static int g_trialUsedSec = 0;
static bool g_licensed = true;
static bool g_tvBusy = false;      // 电视已被另一台电脑占用
static bool g_tvLost = false;      // 电视端退出/关机（状态回传中断）

// ---------- 编码器配置记忆 ----------
// 某些 Intel QSV 驱动「创建成功但一帧不出」，探测要 2~3 秒；把上次可用的组合记下来，
// 下次启动直接用它，避免每次都重走失败探测（实测新机器要 5~7 秒才出第一帧）。
// 格式： "sw|1"（软件编码器 + GPU 转换） / "hw|2"（硬件 + 纹理直入）
static void EncProfileLoad(bool& sw, int& mode) {
  sw = false; mode = -1;
  std::string s = lic::store_read("encoder.cfg");
  if (s.size() < 3) return;
  size_t bar = s.find('|');
  if (bar == std::string::npos) return;
  int m = atoi(s.c_str() + bar + 1);
  if (m < 0 || m > 2) return;
  sw = (s.substr(0, bar) == "sw");
  mode = m;
}
static void EncProfileSave(bool sw, int mode) {
  char b[32];
  snprintf(b, sizeof b, "%s|%d", sw ? "sw" : "hw", mode);
  lic::store_write("encoder.cfg", b);
}


int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  SetConsoleOutputCP(65001);
  // 进程级 DPI 感知：DDA 的帧是物理像素，光标坐标/系统尺寸也必须按物理像素解释，
  // 否则 125% 缩放下光标位置会偏 1/1.25（实测 GetSystemMetrics 返回 2048x1152 而非 2560x1440）。
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  Cfg cfg = ParseArgs(argc, argv);
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--machinecode") { printf("%s\n", lic::machine_code().c_str()); return 0; }
    if (std::string(argv[i]) == "--machinedbg")  { printf("%s\n", lic::machine_debug().c_str()); return 0; }
  }

  // ---------- 授权（原生验签：界面被改写也绕不过）----------
  {
    std::string licMsg; bool licensed = false;
    std::string codeOverride;
    for (int i = 1; i + 1 < argc; i++)
      if (std::string(argv[i]) == "--license") codeOverride = argv[i + 1];
    if (!codeOverride.empty()) {
      // 只写入「验签通过」的码：旧版无条件 store_write，任何来源（含伪造信标注入的命令行）
      // 都能用垃圾码覆盖用户已生效的授权文件，表现为"昨天还能用今天变未注册"。
      std::string why; bool perm = false;
      if (lic::verify(codeOverride, why, perm)) {
        lic::store_write("license.dat", codeOverride);
      } else {
        fprintf(stderr, "IGNORED_LICENSE: %s\n", why.c_str());
      }
    }

    int rc = lic::check_start(licMsg, licensed);
    if (rc != 0) {
      Logf("授权校验失败：未激活且今日试用（每天 5 分钟）已用完，请在电脑端注册后重试。");
      fprintf(stderr, "LICENSE_REQUIRED\n");
      return 3;
    }
    g_licensed = licensed;
    g_trialUsedSec = licensed ? 0 : lic::trial_used();
    Logf("授权状态：%s（今日试用已用 %d 秒）", licensed ? "已授权" : "试用中", g_trialUsedSec);
  }
  SetConsoleCtrlHandler(CtrlHandler, TRUE);
  g_noenc = getenv("PJS_NOENC") != nullptr;
  g_nosend = getenv("PJS_NOSEND") != nullptr;
  g_slowlog = getenv("PJS_SLOWLOG") != nullptr;
  g_curdbg = getenv("PJS_CURSORDBG") != nullptr;
  logx::Init(cfg.logPath, (size_t)cfg.logSizeMB << 20);
  SetUnhandledExceptionFilter(logx::CrashFilter);
  logx::Logf("=== Projector sender start ===  log=%s  max=%dMB", logx::g_path.c_str(), cfg.logSizeMB);

  // ---------- 设备发现：未指定 --ip 时监听 TV 的 UDP 45691 信标 ----------
  if (cfg.ip.empty()) {
    Logf("正在搜索接收端（UDP %d，最多 2 秒）...\n", pjs::DiscoverPort());
    auto devs = pjs::Discover(2000);
    if (devs.empty()) {
      Logf("未发现接收端。请确认：\n"
             "  1) 电视上的 Projector 接收端已启动\n"
             "  2) PC 防火墙放行入站 UDP %d（管理员: netsh advfirewall firewall add rule "
             "name=\"Projector Discovery\" dir=in action=allow protocol=UDP localport=%d）\n"
             "或直接用 --ip <电视IP> 指定。\n", pjs::DiscoverPort(), pjs::DiscoverPort());
      return 1;
    }
    for (size_t i = 0; i < devs.size(); i++)
      Logf("接收端[%zu] %s  %s:%d%s\n", i, devs[i].name.c_str(), devs[i].ip.c_str(), devs[i].port,
             devs[i].model.empty() ? "" : ("  (" + devs[i].model + ")").c_str());
    cfg.ip = devs[0].ip;
    cfg.port = devs[0].port;
    Logf("使用 %s:%d（可用 --ip 覆盖）\n", cfg.ip.c_str(), cfg.port);
  }

  pjs::UdpOut out;
  if (!out.open(cfg.ip.c_str(), cfg.port)) { Logf("UDP 打开失败\n"); return 1; }
  out.send_hello();                                  // 告诉接收端「新会话开始」
  {                                                   // 上报计算机名，电视端显示"源设备 名称 · IP"
    char hn[64]{}; DWORD hnl = sizeof hn;
    if (GetComputerNameA(hn, &hnl) && hnl > 0) { out.send_hostname(hn); Logf("计算机名: %s\n", hn); }
  }
  if (cfg.switched) { for (int i = 0; i < 6; i++) { out.send_switched(); Sleep(30); } }   // 显示器回退提示
  fb::Feedback feedback;
  bool fbOk = feedback.Start();
  if (!fbOk) Logf("回传端口 45692 绑定失败（电视端状态将不可见）\n");
  Logf("目标 %s:%d  码率 %dkbps  fps %d  偏好%s硬编\n", cfg.ip.c_str(), cfg.port,
         cfg.bitrate, cfg.fps, cfg.hw ? "" : "（禁）");
  if (FAILED(MFStartup(MF_VERSION))) Logf("MFStartup 失败（编码可能不可用）\n");

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
    if (FAILED(hr)) { Logf("D3D11 设备创建失败 0x%08lx\n", hr); return 2; }

    // ---------- 显示器选择：--display N（按枚举顺序，主屏优先）→ GraphicsCaptureItem / Duplication ----------
    struct MonPick { HMONITOR h; RECT rc; wchar_t dev[CCHDEVICENAME]; bool primary; };
    std::vector<MonPick> mons;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR h, HDC, LPRECT, LPARAM p) -> BOOL {
      auto* v = reinterpret_cast<std::vector<MonPick>*>(p);
      MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
      if (GetMonitorInfoW(h, &mi)) {
        MonPick m{}; m.h = h; m.rc = mi.rcMonitor;
        wcsncpy_s(m.dev, mi.szDevice, _TRUNCATE);
        m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        v->push_back(m);
      }
      return TRUE;
    }, (LPARAM)&mons);
    // 主屏排到最前，其余按枚举顺序
    std::stable_sort(mons.begin(), mons.end(), [](const MonPick& a, const MonPick& b) {
      return a.primary && !b.primary;
    });
    if (mons.empty()) { Logf("找不到任何显示器\n"); return 2; }
    UINT idx = (cfg.display >= 0 && (UINT)cfg.display < mons.size()) ? (UINT)cfg.display : 0;
    for (size_t i = 0; i < mons.size(); i++)
      Logf("显示器[%zu] %ls %ldx%ld%s%s\n", i, mons[i].dev,
             mons[i].rc.right - mons[i].rc.left, mons[i].rc.bottom - mons[i].rc.top,
             mons[i].primary ? " (主屏)" : "", i == idx ? "  ← 选中" : "");
    HMONITOR mon = mons[idx].h;
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi); GetMonitorInfoW(mon, &mi);
    bool useDDA = false;
    ComPtr<IDXGIOutputDuplication> dup;
    caps::GraphicsCaptureItem item{ nullptr };
    try {
      auto factory = winrt::get_activation_factory<caps::GraphicsCaptureItem>();
      auto interop = factory.as<IGraphicsCaptureItemInterop>();
      winrt::check_hresult(interop->CreateForMonitor(mon, winrt::guid_of<caps::GraphicsCaptureItem>(),
                                                     winrt::put_abi(item)));
    } catch (winrt::hresult_error& e) {
      Logf("WGC 不可用 0x%08lx，改用 DesktopDuplication\n", (unsigned long)e.code());
      useDDA = true;
    }
    LONG outL = 0, outT = 0;   // 被捕获输出在桌面坐标系中的左上角（光标位置是桌面坐标）
    auto initDDA = [&]() -> HRESULT {
      ComPtr<IDXGIDevice> dg1; dev.As(&dg1); ComPtr<IDXGIAdapter> ad1;
      if (FAILED(dg1->GetAdapter(&ad1))) return E_FAIL;
      for (UINT oi = 0;; oi++) {
        ComPtr<IDXGIOutput> o;
        if (ad1->EnumOutputs(oi, &o) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC od; o->GetDesc(&od);
        if (od.Monitor != mon) continue;
        outL = od.DesktopCoordinates.left; outT = od.DesktopCoordinates.top;
        ComPtr<IDXGIOutput1> o1; o.As(&o1);
        dup.Reset();
        return o1->DuplicateOutput(dev.Get(), &dup);
      }
      return DXGI_ERROR_NOT_FOUND;   // 适配器上找不到该显示器对应输出
    };
    if (useDDA) {
      HRESULT hd = initDDA();
      if (FAILED(hd)) { Logf("DuplicateOutput 失败 0x%08lx\n", hd); fprintf(stderr, "CAPTURE_INIT_FAILED 0x%08lx\n", (unsigned long)hd); return 6; }
    }
    // DDA 纹理是物理像素；rcMonitor 是 DPI 逻辑值（125% 缩放下会小 1/4），用 DEVMODE 取真实尺寸
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    bool haveDM = useDDA && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)
                  && dm.dmPelsWidth && dm.dmPelsHeight;
    if (useDDA && !haveDM) Logf("DDA: EnumDisplaySettings(%ls) 失败，退回逻辑分辨率\n", mi.szDevice);
    int w = useDDA ? (int)(haveDM ? (LONG)dm.dmPelsWidth
                                  : (LONG)(mi.rcMonitor.right - mi.rcMonitor.left))
                   : (int)item.Size().Width;
    int h = useDDA ? (int)(haveDM ? (LONG)dm.dmPelsHeight
                                  : (LONG)(mi.rcMonitor.bottom - mi.rcMonitor.top))
                   : (int)item.Size().Height;
    // 编码分辨率：--height N 时整数倍降采样（CPU 转换循环步进采样）
    int s = 1;
    int ew = w & ~1, eh = h & ~1;
    if (cfg.height > 0 && cfg.height < h) {
      // 目标高度：等比缩放到 cfg.height（宽按比例）。VP 路径能直接输出任意尺寸；
      // CPU 路径只能整数倍抽样，退化到最近的整数步长。旧逻辑要求 cfg.height*2<=h，
      // 导致 1080p 选 720 完全不降采样（1440>1080），界面却显示已降分辨率。
      int ratio = h / cfg.height;
      if (ratio <= 1) {
        ew = ((int)((long long)w * cfg.height / h)) & ~1;
        eh = cfg.height & ~1;
        if (ew < 2) ew = 2;
      } else {
        s = ratio > 8 ? 8 : ratio;
        ew = (w / s) & ~1; eh = (h / s) & ~1;
      }
    }
    Logf("捕获 %dx%d  编码 %dx%d (1/%d)\n", w, h, ew, eh, s);

    // ---------- GPU VideoProcessor：一步 BGRA→NV12 + 缩放（需设备带 VIDEO_SUPPORT） ----------
    ComPtr<ID3D11VideoDevice> vdev; ComPtr<ID3D11VideoContext> vctx;
    ComPtr<ID3D11VideoProcessor> vproc; ComPtr<ID3D11VideoProcessorEnumerator> vpen;
    ComPtr<ID3D11VideoProcessorOutputView> vpOut;
    ComPtr<ID3D11Texture2D> vpTex, stageNV12, stageClean;
    bool vp1 = false;
    do {
      if (!cfg.hw && getenv("PJS_VP_FORCE") == nullptr) break;
      if (FAILED(dev.As(&vdev)) || FAILED(ctx.As(&vctx))) { Logf("VP: 设备无 VIDEO_SUPPORT，走 CPU 转换\n"); break; }
      D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
      cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
      cd.InputFrameRate = cd.OutputFrameRate = { (UINT32)cfg.fps, 1 };
      cd.InputWidth = (UINT)w; cd.InputHeight = (UINT)h;
      cd.OutputWidth = (UINT)ew; cd.OutputHeight = (UINT)eh;
      cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
      if (FAILED(vdev->CreateVideoProcessorEnumerator(&cd, &vpen))) { Logf("VP: 枚举失败\n"); break; }
      UINT vfs = 0;   // SDK 头此处声明为 UINT*（非枚举指针）
      if (FAILED(vpen->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &vfs)) ||
          !(vfs & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) { Logf("VP: 不支持 NV12 输出\n"); break; }
      if (FAILED(vdev->CreateVideoProcessor(vpen.Get(), 0, &vproc))) { Logf("VP: 处理器创建失败\n"); break; }
      D3D11_TEXTURE2D_DESC td{};
      td.Width = ew; td.Height = eh; td.MipLevels = 1; td.ArraySize = 1;
      td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      if (FAILED(dev->CreateTexture2D(&td, nullptr, &vpTex))) { Logf("VP: 输出纹理失败\n"); break; }
      D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{}; ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
      if (FAILED(vdev->CreateVideoProcessorOutputView(vpTex.Get(), vpen.Get(), &ovd, &vpOut))) break;
      D3D11_TEXTURE2D_DESC sd2 = td;
      // CPU_ACCESS_WRITE：叠加鼠标指针要往映射内存里写；只申请 READ 时写操作会被丢弃
      sd2.Usage = D3D11_USAGE_STAGING; sd2.BindFlags = 0;
      sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
      dev->CreateTexture2D(&sd2, nullptr, &stageNV12);   // 可选：内存编码器场景的 NV12 回读
      dev->CreateTexture2D(&sd2, nullptr, &stageClean);  // 同上但不叠加光标：静止补帧时重新叠当前光标
      // 色彩空间：输入 RGB full range（屏幕 sRGB）→ 输出 BT.601 limited（与 CPU 转换系数一致）
      // SDK UM 头无 DXG_COLOR_SPACE 常量，按 dxgityp.h 打包值手填：RGB|FULL=0x81；YCbCr601|LIMITED=0x02
      D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCS{}, outCS{};
      uint32_t vi = 0x00000081, vo = 0x00000002;
      memcpy(&inCS, &vi, 4); memcpy(&outCS, &vo, 4);
      vctx->VideoProcessorSetStreamFrameFormat(vproc.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
      vctx->VideoProcessorSetStreamColorSpace(vproc.Get(), 0, &inCS);
      vctx->VideoProcessorSetOutputColorSpace(vproc.Get(), &outCS);
      vp1 = true;
    } while (0);

    // VP 输入视图缓存：WGC 帧池约 3 块纹理，固定 4 槽复用（池生命周期内指针键有效）
    struct IVE { ID3D11Texture2D* tex; ComPtr<ID3D11VideoProcessorInputView> iv; };
    IVE ivc[4]; int ivn = 0;
    auto getIV = [&](ID3D11Texture2D* t) -> ID3D11VideoProcessorInputView* {
      if (!vp1) return nullptr;
      for (int i = 0; i < ivn; i++) if (ivc[i].tex == t) return ivc[i].iv.Get();
      if (ivn >= 4) return nullptr;
      D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{}; ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
      if (SUCCEEDED(vdev->CreateVideoProcessorInputView(t, vpen.Get(), &ivd, &ivc[ivn].iv))) {
        ivc[ivn].tex = t; return ivc[ivn++].iv.Get();
      }
      return nullptr;
    };

    // VP 输出视图缓存：编码器环纹理（16 槽）用——VP 直出到编码器输入，省掉每帧一次
    // 1440p NV12 CopyResource（原路：Blt→vpTex→CopyResource→环纹理）。
    struct OVE { ID3D11Texture2D* tex; ComPtr<ID3D11VideoProcessorOutputView> ov; };
    OVE ovc[20]; int ovn = 0;
    auto getOV = [&](ID3D11Texture2D* t) -> ID3D11VideoProcessorOutputView* {
      for (int i = 0; i < ovn; i++) if (ovc[i].tex == t) return ovc[i].ov.Get();
      if (ovn >= 20) return nullptr;
      D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{}; ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
      if (SUCCEEDED(vdev->CreateVideoProcessorOutputView(t, vpen.Get(), &ovd, &ovc[ovn].ov))) {
        ovc[ovn].tex = t; return ovc[ovn++].ov.Get();
      }
      return nullptr;
    };

    // ---------- 帧池 + 会话（仅 WGC 路径；DDA 回退时为空） ----------
    ComPtr<IDXGIDevice> dxgi; dev.As(&dxgi);
    caps::Direct3D11CaptureFramePool pool{ nullptr };
    caps::GraphicsCaptureSession session{ nullptr };
    if (!useDDA) {
      g11::IDirect3DDevice d3ddev{ nullptr };
      winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(),
          reinterpret_cast<IInspectable**>(winrt::put_abi(d3ddev))));
      pool = caps::Direct3D11CaptureFramePool::CreateFreeThreaded(
          d3ddev, gdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, gfx::SizeInt32{ w, h });
      session = pool.CreateCaptureSession(item);
      try { session.IsCursorCaptureEnabled(true); } catch (...) {}
      try { session.IsBorderRequired(false); } catch (...) {}
    }

    // ---------- staging（CPU 转换路径用；mode≥1 时闲置） ----------
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
    if (FAILED(hr2)) { fprintf(stderr, "CAPTURE_INIT_FAILED staging 0x%08lx\n", (unsigned long)hr2); return 6; }
    std::vector<uint8_t> yuv((size_t)ew * eh * 3 / 2);  // CPU 转出的 packed NV12（行距=ew）

    // ---------- device manager + 编码器（与 VP 纹理同一设备，硬编可零拷贝吃纹理） ----------
    ComPtr<IMFDXGIDeviceManager> dxgm; UINT dxgtok = 0;
    if (vp1) {
      if (SUCCEEDED(MFCreateDXGIDeviceManager(&dxgtok, &dxgm)) && FAILED(dxgm->ResetDevice(dev.Get(), dxgtok)))
        dxgm.Reset();
    }
    H264Encoder enc;
    bool profSw = false; int profMode = -1;
    EncProfileLoad(profSw, profMode);
    bool wantHw = cfg.hw && !profSw;
    if (profSw) Logf("编码器: 上次记录硬件不可用，直接用软件编码器（跳过探测）\n");
    if (!enc.Create(ew, eh, cfg.fps, cfg.bitrate, wantHw, dxgm.Get(), dxgtok)) {
      if (wantHw) {
        Logf("编码器: 硬件创建失败，改用软件编码器\n");
        if (!enc.Create(ew, eh, cfg.fps, cfg.bitrate, false, nullptr, 0)) { Logf("H.264 编码器创建失败\n"); return 4; }
      } else { Logf("H.264 编码器创建失败\n"); return 4; }
    }
    int mode = 0;   // 输入路径: 0=CPU 转换 1=GPU 转换+NV12 回读 2=GPU 纹理直入
    int texfail = 0;
    int memfail = 0;   // 模式1(NV12 内存输入)连续失败次数
    // 输入路径由实测帧间隔抖动决定：默认 NV12 回读（mem 输入，稳定）；
    // 设 PJS_TEXTURE=1 可切到纹理直入（少两次 5.5MB 搬运，但 MFT 读纹理要等 GPU）。
    if (vp1) {
      bool wantTex = enc.d3d() && getenv("PJS_TEXTURE") != nullptr;
      mode = wantTex ? 2 : (stageNV12 ? 1 : 0);
    }
    if (profMode >= 0 && !(profMode == 2 && !enc.d3d())) mode = profMode;   // 沿用上次成功的输入方式
    Logf("编码器: %s  输入: %s\n", enc.hw() ? "硬件(MFT)" : "软件(MS)",
           mode == 2 ? "GPU 纹理(零拷贝)" : mode == 1 ? "GPU 转换+NV12 拷贝" : "CPU 转换");

    // ---------- M3 音频: WASAPI loopback → AAC(128k) → PJS ----------
    LARGE_INTEGER qpf{}; QueryPerformanceFrequency(&qpf);   // 必须先于音频 lambda 声明（&qpf 按引用捕获）
    aud::AudioPipe apipe;
    bool audioOn = false;
    // 运行时开关：界面勾/取消「传输声音」→ 写共享内存，这里每秒检查一次，立即生效
    HANDLE ctlMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 64,
                                       L"Local\\XingLianCastCtl");
    volatile LONG* ctl = ctlMap ? (volatile LONG*)MapViewOfFile(ctlMap, FILE_MAP_ALL_ACCESS, 0, 0, 64) : nullptr;
    if (ctl) { ctl[0] = cfg.audio ? 1 : 0; ctl[1] = 0; }
    auto setAudio = [&](bool on) {
      if (on == audioOn) return;
      if (on) {
        apipe.Start(128, [&](const uint8_t* a, size_t n, uint64_t ts) { out.send_audio(a, n, ts); });
        audioOn = true;
        Sleep(250);
        if (apipe.started()) Logf("音频: 已开启  loopback+AAC-128k\n");
        else if (apipe.err() == S_OK) Logf("音频: 启动中\n");
        else Logf("音频: 不可用(仅视频) hr=0x%08X\n", (unsigned)apipe.err());
      } else {
        apipe.Stop();
        audioOn = false;
        Logf("音频: 已关闭（仅视频）\n");
      }
    };
    setAudio(cfg.audio);

    // ---------- 输出回调 ----------
    std::vector<uint8_t> cfg_sps;      // SPS/PPS（Annex-B）供 meta
    uint64_t frames = 0, t0 = GetTickCount64();
    uint64_t total_au = 0;                     // 累计编码出的帧数（诊断用，秒级 frames 会清零）
    const uint64_t startMs = GetTickCount64();
    uint64_t lastRescueMs = startMs;   // 编码器看门狗：上一次尝试切换输入/编码器的时间
    uint64_t lastIdrMs = GetTickCount64();             // 上一次发出关键帧的时间
    uint64_t lastAuMs = GetTickCount64();              // 上一次真正编码出 AU 的时间（看门狗用）
    bool needIdr = false;                              // 需要立刻补一个 IDR（丢包/首帧丢失）
    bool profSaved = false;                            // 是否已把可用编码器配置落盘
    int lastSecFrames = 0;                             // 上一秒实际编码帧数（判断桌面是否静止）
    auto on_au = [&](const uint8_t* au, size_t n, bool key, bool idr) {
      LARGE_INTEGER q; QueryPerformanceCounter(&q);
      uint64_t ts_us = (uint64_t)q.QuadPart * 1000000 / (uint64_t)qpf.QuadPart;
      if (cfg_sps.empty()) { auto c = pjs::extract_cfg(au, n); if (!c.empty()) cfg_sps = std::move(c); }
      if (!g_nosend) out.send_au(au, n, ts_us, key, idr);
      if (key) lastIdrMs = GetTickCount64();
      frames++; total_au++;
    };

    if (session) { try { session.StartCapture(); } catch (...) {} }
    Logf("已推流: 按 Ctrl+C 退出\n");

    const int safeFps = cfg.fps > 0 ? cfg.fps : 60;   // 双保险：任何路径都不允许除零
    LONGLONG dur100 = 10000000 / safeFps;    uint64_t frame_int_us = 1000000 / (uint64_t)safeFps;  // 限帧间隔（QPC 微秒，tick 分辨率 15.6ms 不够）
    uint64_t last_enc = 0, last_in = 0, last_meta = 0, last_keep = 0, last_ping = 0, conv_us = 0, enc_us = 0, last_send_err = 0;
    uint64_t last_bytes = 0, last_abytes = 0;   // 每秒流量统计
    bool haveFrame = false; uint64_t lastSendMs = 0;   // 静止画面补 IDR 用
    // 锁屏暂停帧：把最后一帧强模糊后循环发送，避免电视解码器饿死出现绿屏
    std::vector<uint8_t> pauseFrame; int pauseW = 0, pauseH = 0;
    bool havePauseFrame = false, pauseFirstIdr = true;

    uint64_t n_acq = 0, n_lim = 0;   // 诊断：捕获源给帧数 / 被限帧丢弃数（每秒清零）
    uint64_t totalAcq = 0, totalLim = 0;   // 累计值，用于"始终没有画面"判断
    uint64_t win_us = 0, win_n = 0;  // （保留字段，令牌桶已取代）
    double tb_tokens = 0.0; uint64_t tb_last = 0;
    uint64_t gap_max = 0, gap_big = 0;
    LONGLONG last_t100 = 0; uint64_t tgap_max = 0;
    bool first = true;

    // ---------- 单帧处理：接手捕获源的 BGRA 纹理（WGC / DesktopDuplication 共用） ----------
    auto process = [&](ID3D11Texture2D* srcTex, LONGLONG t100) {
      LARGE_INTEGER qi; QueryPerformanceCounter(&qi);
      uint64_t nowus = (uint64_t)qi.QuadPart * 1000000 / (uint64_t)qpf.QuadPart;
      // 限帧改成「每秒令牌桶」：按到达间隔判断会因循环抖动把 16.7ms 的帧算成 15.5ms，
      // 实测每秒误杀 4~10 帧（收=60 / 限=7 / fps=52）。令牌桶只约束平均速率，
      // 60Hz 源全部放行；真・高刷源（120Hz）仍被压到 --fps。
      if (!first) {
        uint64_t gap = nowus - last_in;
        if (gap > gap_max) gap_max = gap;
        if (gap > 25000) gap_big++;                 // 诊断：>25ms 的帧间隔（视觉上就是卡一下）
        // 令牌桶（连续补充）：每秒 fps 个令牌。不能用「每秒配额」——那会先放行一整秒的帧、
        // 再冻结剩余时间（实测 --fps 30 时出现 484ms 的大间隔）。
        tb_tokens += (double)(nowus - tb_last) * cfg.fps / 1e6;
        if (tb_tokens > 2.0) tb_tokens = 2.0;       // 允许 2 帧突发
        tb_last = nowus;
        if (tb_tokens < 1.0) { n_lim++; totalLim++; return; }
        tb_tokens -= 1.0;
      }
      // 对照：DDA 自带显示时间戳的间隔（100ns）。若它平稳而上面的 nowus 间隔有尖峰，
      // 说明桌面照常出帧、是我们循环晚了；若它也出现同样尖峰，则是桌面/合成器卡。
      if (last_t100 != 0 && t100 > 0) {
        LONGLONG dt = t100 - last_t100;
        if (dt > 0 && (uint64_t)dt > tgap_max) tgap_max = (uint64_t)dt;
      }
      win_n++;
      last_in = nowus;
      last_t100 = t100;
      // 关键帧策略：首帧 / 丢包后立刻补 / 桌面静止（上一秒 <15fps）时每 2 秒补一个。
      // 实测（另一台 PC）：静止桌面下只发 P 帧，一旦首帧 IDR 在链路上丢了，电视会一直黑屏，
      // 直到用户制造画面变化才恢复——就是"TV 显示连接中然后回等待页"的根因。
      bool forceIdr = first || needIdr;
      if (!forceIdr && lastSecFrames < 15 && GetTickCount64() - lastIdrMs > 2000) forceIdr = true;
      if (needIdr) needIdr = false;
      LARGE_INTEGER c0, c1, c2; QueryPerformanceCounter(&c0);
      bool ok = false;
      D3D11_MAPPED_SUBRESOURCE mpGPU{};
      if (mode >= 1) {   // GPU VideoProcessor：BGRA→NV12 + 缩放一步完成
        bool prepared = false;
        ID3D11VideoProcessorInputView* iv = getIV(srcTex);
        if (iv) {
          D3D11_VIDEO_PROCESSOR_STREAM ps{}; ps.Enable = TRUE; ps.pInputSurface = iv;
          // 纹理直入模式：先把 Blt 目标定为编码器环槽位，省掉 Blt→vpTex→环纹理 的一次整帧拷贝
          ID3D11Texture2D* ringTex = (mode == 2) ? enc.NextInputTex(dev.Get()) : nullptr;
          ID3D11VideoProcessorOutputView* ov = vpOut.Get();
          if (ringTex) { if (auto* ro = getOV(ringTex)) ov = ro; }
          if (SUCCEEDED(vctx->VideoProcessorBlt(vproc.Get(), ov, 0, 1, &ps))) {
            if (mode == 2) {
              if (ringTex) {
                if (ov != vpOut.Get()) {
                  prepared = true;                          // 直出：VP 已写进编码器输入纹理
                } else {
                  ctx->CopyResource(ringTex, vpTex.Get());  // 兜底：输出视图建失败时仍走拷贝
                  prepared = true;
                }
              }
            } else {
              ctx->CopyResource(stageNV12.Get(), vpTex.Get());
              if (stageClean) ctx->CopyResource(stageClean.Get(), vpTex.Get());   // 干净帧：静止补帧用，避免重发旧光标
              // 光标：用 DrawIconEx 烘成位图后合成（自己解析掩码在 MASKED_COLOR 上会画成黑块）
              int cw = 0, ch = 0, chx = 0, chy = 0;
              bool curOk = s == 1 && !getenv("PJS_NOCURSOR") && g_cur.visible &&
                           GrabCursorImage(g_curBmp, g_curBmpB, cw, ch, chx, chy);
              const bool wantCur = curOk;
              prepared = SUCCEEDED(ctx->Map(stageNV12.Get(), 0,
                                            wantCur ? D3D11_MAP_READ_WRITE : D3D11_MAP_READ, 0, &mpGPU));
              // 光标叠加在回读的 NV12 上做（整帧已映射好，指针只占 ~1K 像素）
              if (prepared && wantCur)
                BlendCursorImage((uint8_t*)mpGPU.pData, (int)mpGPU.RowPitch, ew, eh,
                                 g_curBmp, g_cur.px, g_cur.py, chx, chy, cw, ch);
            }
          } else { mode = 0; Logf("VP: Blt 失败，改走 CPU 转换\n"); }
        }
        if (prepared) {
          QueryPerformanceCounter(&c1);
          if (g_noenc) {                       // 诊断：只做采集/转换，不编码
            ok = true;
            if (mode == 1) ctx->Unmap(stageNV12.Get(), 0);
          } else if (mode == 2) {
            ok = enc.EncodeGPU(t100, dur100, forceIdr, on_au);
          } else {
            uint8_t* yp = (uint8_t*)mpGPU.pData;
            ok = enc.Encode(yp, (int)mpGPU.RowPitch, yp + (size_t)mpGPU.RowPitch * eh,
                            (int)mpGPU.RowPitch, t100, dur100, forceIdr, on_au,
                            (size_t)mpGPU.RowPitch * eh * 3 / 2);
            ctx->Unmap(stageNV12.Get(), 0);
          }
          QueryPerformanceCounter(&c2);
          conv_us += (uint64_t)(c1.QuadPart - c0.QuadPart) * 1000000 / (uint64_t)qpf.QuadPart;
          enc_us  += (uint64_t)(c2.QuadPart - c1.QuadPart) * 1000000 / (uint64_t)qpf.QuadPart;
        } else if (mode == 1) mode = 0;   // NV12 回读失败 → 本帧 CPU
        if (!ok && mode == 2 && ++texfail >= 2) {
          // 纹理直入也失败：若已经从模式1掉下来过，直接退到 CPU，避免二者来回切换
          mode = (memfail > 0 || !stageNV12) ? 0 : 1;
          texfail = 0;
          Logf("enc: GPU 纹理输入失败，改用 %s\n", mode == 1 ? "GPU 转换+NV12 拷贝" : "CPU 转换");
        }

      }
      if (!ok && mode == 0) {   // CPU 转换路径（--sw 与 VP 失败时保留）
        ctx->CopyResource(staging.Get(), srcTex);   // 同格式 BGRA 拷贝
        D3D11_MAPPED_SUBRESOURCE mp{};
        if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mp))) {
          // BGRA → packed NV12（BT.601 limited range），行距=ew，1/s 步进采样缩放
          QueryPerformanceCounter(&c0);
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
          ok = enc.Encode(Yp, ew, UVp, ew, t100, dur100, forceIdr, on_au);
          QueryPerformanceCounter(&c2);
        }
      }
      if (ok && mode == 0) {
        conv_us += (uint64_t)(c1.QuadPart - c0.QuadPart) * 1000000 / (uint64_t)qpf.QuadPart;
        enc_us  += (uint64_t)(c2.QuadPart - c1.QuadPart) * 1000000 / (uint64_t)qpf.QuadPart;
      }
      // 输入方式回退链：内存 NV12(1) → 纹理直入(2) → CPU(0)。
      // 实测 Intel QSV 带 D3D 管理器时只接受纹理输入，内存样本被 ProcessInput 拒绝，
      // 表现就是 fps=0、编码=0.0ms、TV 一直黑屏；这里任何方式连续 3 帧失败就换下一种。
      if (!ok && !g_noenc) {
        if (++memfail >= 3) {
          memfail = 0;
          texfail = 0;
          if (mode <= 1 && enc.d3d() && dev && enc.NextInputTex(dev.Get()) != nullptr) {
            mode = 2;
            Logf("enc: 连续无输出，输入改走 GPU 纹理直入\n");
          } else if (mode != 0) {
            mode = 0;
            Logf("enc: 连续无输出，输入改走 CPU 转换\n");
          } else {
            Logf("[错误] 编码器连续拒绝所有输入方式（内存/纹理/CPU），请把日志发回排查\n");
          }
        }
      } else if (ok) memfail = 0;
      if (ok) {
        first = false;
        haveFrame = true;
        lastSendMs = GetTickCount64();
        LARGE_INTEGER qe; QueryPerformanceCounter(&qe);
        last_enc = (uint64_t)qe.QuadPart * 1000000 / (uint64_t)qpf.QuadPart;
      }
    };

    // 生成"模糊暂停帧"：锁屏期间循环发送，避免电视解码器饿死（绿屏）并保护隐私
    auto makePauseFrame = [&]() {
      if (!stageNV12 || !ctx) return;
      D3D11_MAPPED_SUBRESOURCE mpp{};
      if (FAILED(ctx->Map(stageNV12.Get(), 0, D3D11_MAP_READ, 0, &mpp))) return;
      pauseW = ew; pauseH = eh;
      pauseFrame.resize((size_t)pauseW * pauseH * 3 / 2);
      for (int r = 0; r < pauseH; r++)
        memcpy(pauseFrame.data() + (size_t)r * pauseW,
               (const uint8_t*)mpp.pData + (size_t)r * mpp.RowPitch, pauseW);
      for (int r = 0; r < pauseH / 2; r++)
        memcpy(pauseFrame.data() + (size_t)pauseW * pauseH + (size_t)r * pauseW,
               (const uint8_t*)mpp.pData + (size_t)mpp.RowPitch * pauseH + (size_t)r * mpp.RowPitch, pauseW);
      ctx->Unmap(stageNV12.Get(), 0);
      // 两轮盒式模糊 ≈ 近似高斯：强度明显高于单轮同半径
      BlurPlane(pauseFrame.data(), pauseW, pauseH, 36);
      BlurPlane(pauseFrame.data(), pauseW, pauseH, 36);
      BlurPlane(pauseFrame.data() + (size_t)pauseW * pauseH, pauseW, pauseH / 2, 24);
      BlurPlane(pauseFrame.data() + (size_t)pauseW * pauseH, pauseW, pauseH / 2, 24);
      havePauseFrame = true; pauseFirstIdr = true;
      Logf("已生成模糊暂停帧 %dx%d\n", pauseW, pauseH);
    };
    // ---------- meta 重发 + 每秒统计（两个捕获循环共用） ----------
  // 试用额度（引擎自己记账，界面改不了）：每 5 秒落盘，用尽则停止推流
  auto trial_accumulate = [&]() {
    if (g_licensed) return;
    static uint64_t last = 0;
    uint64_t nowMs = GetTickCount64();
    if (last == 0) { last = nowMs; return; }
    if (nowMs - last < 1000) return;
    last = nowMs;
    g_trialUsedSec++;
    if (g_trialUsedSec % 5 == 0) lic::trial_set(g_trialUsedSec);
    if (g_trialUsedSec >= lic::kTrialSecondsPerDay) {
      lic::trial_set(lic::kTrialSecondsPerDay);
      Logf("今日试用（每天 5 分钟）已用完，停止投屏。");
      fprintf(stderr, "TRIAL_EXHAUSTED\n");
      g_stop = true;
    }
  };

auto tick = [&]() {
      trial_accumulate();
      uint64_t now = GetTickCount64();
      if (!cfg_sps.empty() && now - last_meta > 2000) {
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        uint64_t ts = (uint64_t)q.QuadPart * 1000000 / (uint64_t)qpf.QuadPart;
        out.send_meta(cfg_sps, ts);
        last_meta = now;
      }
      // 心跳：桌面静止 + --noaudio 时链路上无包，接收端会按空闲超时掉解码器
      if (now - last_keep > 1000) { out.send_keepalive(); last_keep = now; }
      // 每秒 ping 一次，TV 回 pong → 算出网络 RTT
      if (now - last_ping > 1000) { out.send_ping(now); last_ping = now; }
      // 界面的「传输声音」勾选：共享内存里变了就立即生效
      if (ctl) { bool want = ctl[0] != 0; if (want != audioOn) setAudio(want); }
      // 锁屏检测（每秒一次）：进入锁屏 → 暂停抓屏 + 通知电视；解锁 → 重新初始化捕获 + 通知电视
      {
        static bool wasLocked = false;
        static bool needReinitAfterUnlock = false;
        bool nowLocked = InputDesktopLocked();
        if (nowLocked != wasLocked) {
          wasLocked = nowLocked;
          if (nowLocked) {
            makePauseFrame();
            g_paused = true;
            out.send_lock();
            Logf("锁屏：暂停推流（保持心跳）\n");
          } else {
            g_paused = false;
            needReinitAfterUnlock = true;
            out.send_unlock();
            Logf("解锁：恢复推流\n");
          }
        }
        if (needReinitAfterUnlock && !nowLocked) {
          needReinitAfterUnlock = false;
          if (useDDA) {
            HRESULT hri = initDDA();        // DDA：锁屏会让桌面复制失效，必须重建
            Logf("解锁后重新初始化 DDA 捕获 hr=0x%08x\n", (unsigned)hri);
          } else {
            // WGC：捕获会话不受锁屏影响（锁屏期间只是丢帧），不要乱调 initDDA
            Logf("解锁：WGC 捕获继续，从关键帧恢复\n");
          }
          first = true;                     // 两条路径都从关键帧重新开始
          haveFrame = false;
        }
      }
      // 诊断：PJS_FAKE_LOCK=1 → 3 秒发 lk、8 秒发 ul，用来自测电视端遮罩（无需真的锁屏）
      if (getenv("PJS_FAKE_LOCK")) {
        static bool lkSent = false, ulSent = false;
        uint64_t el = now - startMs;
        if (el > 3000 && !lkSent) { lkSent = true; out.send_lock(); Logf("[diag] 发送 lk\n"); }
        if (el > 8000 && !ulSent) { ulSent = true; out.send_unlock(); Logf("[diag] 发送 ul\n"); }
      }
      if (now - t0 > (g_slowlog ? 10000 : 1000)) {
        double sec = (now - t0) / 1000.0;
        uint64_t e = out.send_errors();
        uint64_t nb = out.bytes(), nab = out.audio_bytes();
        double mbps = (nb - last_bytes) * 8.0 / sec / 1e6;          // 实测总带宽
        double akbps = (nab - last_abytes) * 8.0 / sec / 1e3;       // 音频带宽
        last_bytes = nb; last_abytes = nab;
        feedback.Tick(now);
        fb::Snapshot fs = feedback.Get();
        if (fs.busy && !g_tvBusy && !getenv("PJS_IGNOREBUSY")) {
          g_tvBusy = true;
          Logf("[提示] 电视正在被另一台电脑投屏（先连的设备优先），本机停止投屏。");
          fprintf(stderr, "TV_BUSY\n");
          g_stop = true;
        }
        lastSecFrames = (int)frames;
        // 电视端退出/关机检测：本会话曾经收到过状态回传，若连续 5 秒收不到 → 判定已断开。
        // 只看"曾经收到过"是因为老版本电视端可能不发回传，避免误判。
        static bool fbSeen = false;
        static uint64_t lastFbMs = 0;
        if (fs.alive) { fbSeen = true; lastFbMs = now; }
        else if (fbSeen && !g_tvLost && now - lastFbMs > 5000) {
          g_tvLost = true;
          Logf("[提示] 电视端已断开（超过 5 秒没有状态回传），停止投屏。");
          fprintf(stderr, "TV_LOST\n");
          g_stop = true;
        }
        if (fs.alive && fs.lost > 0) needIdr = true;   // 电视报丢包：立刻补 IDR，避免长期黑屏
        if (fs.alive && fs.needIdr) needIdr = true;    // 电视在等关键帧（解码器刚重建）→ 立刻补
        char line[768];
        snprintf(line, sizeof line,
                 "fps=%.1f 收=%llu 限=%llu 间隔max=%.0fms 大间隔=%llu 显示间隔max=%.0fms 包=%llu 音包=%u 流量=%.2fMbps 音流=%.0fkbps 转换=%.1fms 编码=%.1fms 送失=%llu [%s]", frames / sec,
                 (unsigned long long)n_acq, (unsigned long long)n_lim,
                 gap_max / 1000.0, (unsigned long long)gap_big, tgap_max / 10000.0,
                 (unsigned long long)out.packets(), (unsigned)out.audio_packets(), mbps, akbps,
                 frames ? conv_us / frames / 1000.0 : 0.0,
                 frames ? enc_us / frames / 1000.0 : 0.0,
                 (unsigned long long)(e - last_send_err),
                 mode == 2 ? "纹理" : mode == 1 ? "GPU转换" : "CPU");
        if (fs.alive) {   // TV 回传有效时追加接收端指标
          size_t used = strlen(line);
          snprintf(line + used, sizeof line - used, " tvfps=%.1f tvlost=%ld tvdrop=%ld rtt=%.1f",
                   fs.fps, fs.lost, fs.drop, fs.rttMs);
        }
        if (g_curdbg) {
          char cl[96];
          snprintf(cl, sizeof cl, "  游=%d %d,%d %ux%u t%u",
                   g_cur.visible ? 1 : 0, g_cur.px, g_cur.py, g_cur.w, g_cur.h, g_cur.type);
          Logf("%s", cl);
        }
        Logf("%s", line);
        if (!profSaved && total_au > 0) { EncProfileSave(!enc.hw(), mode); profSaved = true; }
        last_send_err = e;
        n_acq = n_lim = 0; gap_max = 0; gap_big = 0; tgap_max = 0;
        t0 = now; frames = 0; conv_us = enc_us = 0;
      }
      // 看门狗：编码器 3 秒一帧都没吐出来（async MFT 可能既不报错也不出帧，实测 Intel QSV）
      // → 依次切换输入方式，最后换成软件编码器，保证一定有画面。
      static int rescueStep = 0;
      // 只覆盖启动期（编码器一帧都没出）。运行中重建编码器实测会崩溃，已回退；
      // 如需重做"中途停摆自救"，必须单独验证该路径。
      if (!g_noenc && total_au == 0 && rescueStep < 3 && now - lastRescueMs > 900) {
        lastRescueMs = now;
        rescueStep++;
        if (rescueStep == 1 && mode != 2 && enc.d3d() && dev && enc.NextInputTex(dev.Get()) != nullptr) {
          mode = 2;
          Logf("enc: 3 秒无输出，输入改走 GPU 纹理直入\n");
        } else if (rescueStep == 2 && mode != 0) {
          mode = 0;
          Logf("enc: 3 秒无输出，输入改走 CPU 转换\n");
        } else if (rescueStep == 3) {
          Logf("enc: 3 秒仍无输出，改用软件编码器（CPU）\n");
          enc.ReleaseForRetry();
          if (!enc.Create(ew, eh, cfg.fps, cfg.bitrate, false, nullptr, 0))
            Logf("[错误] 软件编码器创建失败\n");
          // 关键：软件编码器吃的是内存 NV12，但**转换仍交给 GPU**（mode 1）。
          // 之前退到 mode 0 让 CPU 做 1440p 的 BGRA→NV12（实测 10~14ms/帧），
          // 直接把这台机器压到 30fps；改成 GPU 转换后转换降到 ~2ms。
          mode = stageNV12 ? 1 : 0;
          Logf("enc: 软件编码器就绪，转换走 %s\n", mode == 1 ? "GPU" : "CPU");
        }
      }
      // 诊断：启动 5 秒后依然一帧都没编码出来 → 明确报错并给出上下文，避免"不报错但没画面"
      static bool noFrameWarned = false;
      if (!g_noenc && !noFrameWarned && total_au == 0 && now - startMs > 8000) {
        noFrameWarned = true;
        Logf("[错误] 启动 8 秒仍无视频帧：输出=%dx%d 编码模式=%d 采集到帧=%llu 限帧丢弃=%llu "
             "→ 采集或编码失败（请把本文件发回排查）\n",
             ew, eh, mode, (unsigned long long)totalAcq, (unsigned long long)totalLim);
      }
    };

    // 采集线程优先级：4K 视频软解/浏览器解码线程会抢占时间片，导致桌面呈现停顿
    // 50~110ms（实测 4K60 全屏时 fps 掉到 50~57、个别秒 111ms；资源并未饱和，属调度问题）。
    bool noPrio = getenv("PJS_NOPRIO") != nullptr;   // 诊断：关掉优先级提升做对照
    // 「停止投屏」监听必须无条件安装：旧版被误嵌进 if (!noPrio) 里，
    // 一旦设了 PJS_NOPRIO 就再也收不到停止事件，只能强杀进程（接收端收不到 bye）。
    HANDLE stopEvt = CreateEventW(nullptr, TRUE, FALSE, L"Local\\XingLianCastStop");
    if (stopEvt) {
      ResetEvent(stopEvt);   // 作废上一次残留的信号
      std::thread([stopEvt] { WaitForSingleObject(stopEvt, INFINITE); g_stop = true; }).detach();
    }
    if (!noPrio) {
      SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
      // MMCSS（多媒体调度类）：比单纯提优先级更有效——系统会为采集线程预留带宽，
      // 浏览器解码/合成线程抢不走。任务名用 "Games"（低延迟档）。
      DWORD mmTask = 0;
      HANDLE mmHandle = AvSetMmThreadCharacteristicsW(L"Games", &mmTask);
    }
    if (useDDA) {
      // ---------- DesktopDuplication 取帧循环 ----------
      ComPtr<ID3D11Texture2D> ddaTex;   // 常驻副本纹理（格式/尺寸跟随 Duplication 纹理）
      DXGI_OUTDUPL_FRAME_INFO fi{};
      const DWORD dda_ms = (DWORD)(frame_int_us / 1000);
      while (!g_stop) {
        if (g_paused) {
          // 锁屏期间：每 500ms 重发模糊帧，让电视保持解码（否则绿屏），同时遮罩文字照常显示
          static uint64_t lastPauseTx = 0;
          if (havePauseFrame && mode != 2 && GetTickCount64() - lastPauseTx > 500) {
            lastPauseTx = GetTickCount64();
            LARGE_INTEGER qp; QueryPerformanceCounter(&qp);
            LONGLONG ts = qp.QuadPart * 10000000 / qpf.QuadPart;
            uint8_t* yp = pauseFrame.data();
            uint8_t* uv = yp + (size_t)pauseW * pauseH;
            enc.Encode(yp, pauseW, uv, pauseW, ts, dur100, pauseFirstIdr, on_au);
            pauseFirstIdr = false;
          }
          Sleep(200); tick(); continue;
        }
        // 事件驱动等下一帧：不要 Sleep+轮询——系统定时器粒度 15.6ms，Sleep(16) 常变 31ms，
        // 把循环压到 ~32fps。带超时的 AcquireNextFrame 由 DDA 在桌面合成时唤醒，周期≈刷新率。
        ComPtr<IDXGIResource> res;
        HRESULT da = dup->AcquireNextFrame(dda_ms ? dda_ms : 8, &fi, &res);
        if (da == DXGI_ERROR_ACCESS_LOST) {
          // 锁屏 / 显示模式变化 / 显示器拔线 / RDP 抢占：桌面复制会断开。
          // 过去直接退出（电视回等待页、解锁还要手动重连）；现在进入「暂停」：
          // 保持心跳、通知电视显示遮罩，并每 500ms 尝试恢复，成功后自动继续推流。
          Logf("DDA: 访问丢失（锁屏/显示变化），暂停推流并等待恢复\n");
          out.send_lock();
          bool resumed = false;
          for (int i = 0; i < 7200 && !g_stop; i++) {   // 最多等 1 小时
            Sleep(500);
            tick();                                    // 暂停期间照常心跳/统计
            if (SUCCEEDED(initDDA())) { resumed = true; break; }
          }
          if (!resumed) { Logf("DDA: 等待恢复超时，退出\n"); break; }
          ddaTex.Reset();
          first = true;          // 恢复后必须从关键帧重新开始
          haveFrame = false;
          out.send_unlock();
          Logf("DDA: 已恢复，继续推流\n");
          continue;
        }
        if (da == DXGI_ERROR_WAIT_TIMEOUT) {   // 桌面无新帧
          // 静止画面的「心跳帧」：接收端在 hello/重配后要等一个 IDR 才出画，
          // 而桌面不动就永远没有新帧 → 电视一直黑屏，直到用户动鼠标（实测要等 10s+）。
          // 这里把上一帧的 NV12（还在 staging 纹理里）按 IDR 重编一次。
          if (mode == 2) mode = 1;   // 纹理直入模式下拿不到可复用的干净帧：静止时降到 GPU 转换路径
          if (mode == 2) mode = 1;   // 纹理直入模式拿不到可复用的干净帧：静止时降到 GPU 转换路径
          if (mode == 1 && haveFrame && stageClean && GetTickCount64() - lastSendMs > 500) {
            // 关键：每次补帧前把干净帧重新拷一遍。
            // stageClean 是持久纹理，如果直接往里叠光标，下一次补帧会在**已经叠过的画面**上再叠一次，
            // 半透明边缘被反复合成 → 越叠越黑，最终变成黑块（实测 3~4 次补帧就出现）。
            if (vpTex) ctx->CopyResource(stageClean.Get(), vpTex.Get());
            D3D11_MAPPED_SUBRESOURCE mp{};
            if (SUCCEEDED(ctx->Map(stageClean.Get(), 0,
                                   g_cur.visible ? D3D11_MAP_READ_WRITE : D3D11_MAP_READ, 0, &mp))) {
              int iw = 0, ih = 0, ihx = 0, ihy = 0;
              if (!getenv("PJS_NOCURSOR") && g_cur.visible &&
                  GrabCursorImage(g_curBmp, g_curBmpB, iw, ih, ihx, ihy))
                BlendCursorImage((uint8_t*)mp.pData, (int)mp.RowPitch, ew, eh,
                                 g_curBmp, g_cur.px, g_cur.py, ihx, ihy, iw, ih);
              LARGE_INTEGER q; QueryPerformanceCounter(&q);
              LONGLONG ts = q.QuadPart * 10000000 / qpf.QuadPart;
              uint8_t* yp = (uint8_t*)mp.pData;
              enc.Encode(yp, (int)mp.RowPitch, yp + (size_t)mp.RowPitch * eh, (int)mp.RowPitch,
                         ts, dur100, true, on_au, (size_t)mp.RowPitch * eh * 3 / 2);
              ctx->Unmap(stageClean.Get(), 0);
              lastSendMs = GetTickCount64();
              first = false;
              static int idleN = 0;
              if (getenv("PJS_IDLEDBG") && (++idleN % 10) == 1) Logf("idle refresh #%d\n", idleN);
            }
          }
          tick(); continue;
        }
        if (FAILED(da)) { tick(); continue; }
        n_acq++; totalAcq++;
        ComPtr<ID3D11Texture2D> dt; res.As(&dt);
        // 鼠标指针：形状变化时才重新取（PointerShapeBufferSize>0），位置每帧更新
        if (fi.PointerShapeBufferSize > 0) {
          g_cur.shape.resize(fi.PointerShapeBufferSize);
          DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
          UINT got = 0;
          if (SUCCEEDED(dup->GetFramePointerShape((UINT)g_cur.shape.size(), g_cur.shape.data(), &got, &si))) {
            g_cur.type = si.Type; g_cur.w = si.Width; g_cur.h = si.Height;
            g_cur.pitch = si.Pitch; g_cur.hotX = si.HotSpot.x; g_cur.hotY = si.HotSpot.y;
            if (getenv("PJS_CURSORDUMP")) {   // 诊断：把指针形状原样落盘分析
              FILE* fp = nullptr;
              static bool dumped = false;
              if (!dumped && (fp = fopen("cursor_dump.bin", "wb"))) {
                fwrite(g_cur.shape.data(), 1, got, fp);
                fclose(fp);
                Logf("cursor dump: type=%u w=%u h=%u pitch=%u got=%u\n", si.Type, si.Width, si.Height, si.Pitch, got);
                dumped = true;
              }
            }
          }
        }
        // 鼠标指针位置/可见性：不能只信 DDA 的 PointerPosition——它只在「本帧带指针更新」
        // 时有效，鼠标一停就被清零（实测 游=0 0,0 → 画面上的光标消失）。
        // GetCursorInfo 是系统级状态，静止时依然有效；形状仍用 DDA 的（更清晰）。
        CURSORINFO ci{}; ci.cbSize = sizeof(ci);
        if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
          g_cur.visible = true;
          g_cur.px = (int)ci.ptScreenPos.x - outL;
          g_cur.py = (int)ci.ptScreenPos.y - outT;
        } else {
          g_cur.visible = false;
        }
        LONGLONG t100 = fi.LastPresentTime.QuadPart > 0
            ? fi.LastPresentTime.QuadPart
            : [] { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }() * 10000000 / qpf.QuadPart;
        // 直接处理 DDA 纹理，不再整帧 CopyResource 一份副本（2560x1440 BGRA = 14.7MB/帧，
        // 60fps 下 ~880MB/s 的纯 GPU 拷贝，实测把合成器压到每几秒一次 50~100ms 停顿）。
        // 安全性：Blt/拷贝都在本帧内提交，ReleaseFrame 之后才 Acquire 下一帧（≥16ms），
        // GPU 侧早以完成对这块表面的读取。
        process(dt.Get(), t100);
        dup->ReleaseFrame();
        tick();
      }
    } else {
      // ---------- WGC 取帧循环 ----------
      winrt::handle evt{ CreateEventW(nullptr, FALSE, FALSE, nullptr) };
      pool.FrameArrived([&](auto&&, auto&&) { SetEvent(evt.get()); });
      while (!g_stop) {
        WaitForSingleObject(evt.get(), 250);
        if (g_paused) {
          // 锁屏暂停（H1）：WGC 抓到的是"锁屏界面/登录页"，绝不能推给电视。
          // 这里把真实帧全部丢弃，只按 DDA 路径同样的节奏重发模糊遮罩帧，
          // 保证解码器不死、电视显示"电脑已锁屏"。
          while (auto drop = pool.TryGetNextFrame()) drop.Close();
          static uint64_t lastPauseTxW = 0;
          if (havePauseFrame && mode != 2 && GetTickCount64() - lastPauseTxW > 500) {
            lastPauseTxW = GetTickCount64();
            LARGE_INTEGER qp; QueryPerformanceCounter(&qp);
            LONGLONG ts = qp.QuadPart * 10000000 / qpf.QuadPart;
            uint8_t* yp = pauseFrame.data();
            uint8_t* uv = yp + (size_t)pauseW * pauseH;
            enc.Encode(yp, pauseW, uv, pauseW, ts, dur100, pauseFirstIdr, on_au);
            pauseFirstIdr = false;
          }
          tick();
          continue;
        }
        while (auto frame = pool.TryGetNextFrame()) {
          auto surface = frame.Surface();
          dx11i::IDirect3DDxgiInterfaceAccess* rawacc = nullptr;
          if (SUCCEEDED(surface.as(__uuidof(dx11i::IDirect3DDxgiInterfaceAccess), (void**)&rawacc))) {
            ComPtr<dx11i::IDirect3DDxgiInterfaceAccess> acc; acc.Attach(rawacc);
            ComPtr<ID3D11Texture2D> srcTex;
            if (SUCCEEDED(acc->GetInterface(IID_PPV_ARGS(&srcTex)))) {
              n_acq++; totalAcq++;
              LONGLONG t100 = (LONGLONG)frame.SystemRelativeTime().count();
              if (t100 <= 0) { LARGE_INTEGER q; QueryPerformanceCounter(&q); t100 = q.QuadPart * 10000000 / qpf.QuadPart; }
              process(srcTex.Get(), t100);
            }
          }
          frame.Close();
        }
        tick();
      }
    }
    if (session) { try { session.Close(); } catch (...) {} }
    out.send_bye();                                 // 通知接收端立即断开并释放解码器
    Sleep(40);
    Logf("退出。共发 %llu 包\n", (unsigned long long)out.packets());
    // ---- 统一收尾：旧版直接 return，WASAPI 线程 / 共享内存 / MF 都没释放 ----
    try { apipe.Stop(); } catch (...) {}              // 音频线程必须在进程退出前 join
    if (stopEvt) { CloseHandle(stopEvt); stopEvt = nullptr; }
    if (ctl) { UnmapViewOfFile((LPCVOID)ctl); ctl = nullptr; }
    if (ctlMap) { CloseHandle(ctlMap); ctlMap = nullptr; }
    MFShutdown();
  } catch (winrt::hresult_error& e) {
    Logf("WinRT 异常 0x%08lx: %ls\n", (unsigned long)e.code(), e.message().c_str());
    logx::Shutdown();
    return 9;
  }
  logx::Shutdown();
  if (g_tvBusy) return 5;          // 电视被另一台电脑占用
  if (g_tvLost) return 7;          // 电视端退出/关机，状态回传中断
  return 0;
}
