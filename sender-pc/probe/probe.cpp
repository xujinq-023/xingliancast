// 诊断探针：枚举每个 HMONITOR 的 GraphicsCaptureItem::CreateForMonitor 结果 + 适配器映射
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <vector>
#include <winrt/base.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <Windows.Graphics.Capture.Interop.h>

namespace caps = winrt::Windows::Graphics::Capture;
using Microsoft::WRL::ComPtr;

static IGraphicsCaptureItemInterop* g_interop = nullptr;

static BOOL CALLBACK EnumMon(HMONITOR mon, HDC, LPRECT rc, LPARAM) {
  MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, &mi);
  char devA[128]{}; WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, devA, sizeof(devA), nullptr, nullptr);
  printf("MON %S primary=%d rect=(%ld,%ld)-(%ld,%ld) dev=%s\n",
         mi.szDevice, (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0,
         mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom, devA);
  fflush(stdout);
  caps::GraphicsCaptureItem item{ nullptr };
  HRESULT hr = g_interop->CreateForMonitor(mon, winrt::guid_of<caps::GraphicsCaptureItem>(),
                                           winrt::put_abi(item));
  printf("    CreateForMonitor hr=0x%08lx %s size=%dx%d\n", hr,
         SUCCEEDED(hr) ? "OK" : "失败",
         SUCCEEDED(hr) ? (int)item.Size().Width : 0,
         SUCCEEDED(hr) ? (int)item.Size().Height : 0);
  fflush(stdout);
  return TRUE;
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  SetConsoleOutputCP(65001);
  winrt::init_apartment(winrt::apartment_type::multi_threaded);

  ComPtr<IDXGIFactory1> f1;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f1)))) {
    for (UINT i = 0;; i++) {
      ComPtr<IDXGIAdapter1> a;
      if (f1->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC2 d{}; ComPtr<IDXGIAdapter2> a2; a.As(&a2);
      if (a2) a2->GetDesc2(&d);
      char nameA[128]{}; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, nameA, sizeof(nameA), nullptr, nullptr);
      printf("ADAPTER[%u] %s  VEN=%04x DEV=%04x  LUID=%ld:%ld  VRAM=%lluMB\n", i, nameA,
             d.VendorId, d.DeviceId, d.AdapterLuid.HighPart, d.AdapterLuid.LowPart,
             (unsigned long long)(d.DedicatedVideoMemory >> 20));
      for (UINT j = 0;; j++) {
        ComPtr<IDXGIOutput> o;
        if (a->EnumOutputs(j, &o) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC od; o->GetDesc(&od);
        char dn[128]{}; WideCharToMultiByte(CP_UTF8, 0, od.DeviceName, -1, dn, sizeof(dn), nullptr, nullptr);
        printf("    OUT[%u] %s  desktop=(%ld,%ld)-(%ld,%ld) mon=%p\n", j, dn,
               od.DesktopCoordinates.left, od.DesktopCoordinates.top,
               od.DesktopCoordinates.right, od.DesktopCoordinates.bottom, (void*)od.Monitor);
      }
    }
  } else printf("CreateDXGIFactory1 失败\n");

  auto factory = winrt::get_activation_factory<caps::GraphicsCaptureItem>();
  g_interop = factory.as<IGraphicsCaptureItemInterop>().get();

  printf("--- 逐个 HMONITOR 试捕获 ---\n");
  EnumDisplayMonitors(nullptr, nullptr, EnumMon, 0);

  printf("--- 试 CreateForWindow(桌面窗口) ---\n");
  HWND hwnd = GetShellWindow();
  if (!hwnd) hwnd = GetDesktopWindow();
  if (hwnd) {
    caps::GraphicsCaptureItem item{ nullptr };
    HRESULT hr = g_interop->CreateForWindow(hwnd, winrt::guid_of<caps::GraphicsCaptureItem>(), winrt::put_abi(item));
    printf("CreateForWindow hr=0x%08lx %s\n", hr, SUCCEEDED(hr) ? "OK" : "失败");
  }

  printf("--- 试 DesktopDuplication ---\n");
  ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
  if (SUCCEEDED(hr)) {
    ComPtr<IDXGIDevice> dg; dev.As(&dg); ComPtr<IDXGIAdapter> ad; dg->GetAdapter(&ad);
    ComPtr<IDXGIOutput> out1;
    if (SUCCEEDED(ad->EnumOutputs(0, &out1))) {
      ComPtr<IDXGIOutput1> o1; out1.As(&o1);
      ComPtr<IDXGIOutputDuplication> dup;
      hr = o1->DuplicateOutput(dev.Get(), &dup);
      printf("DuplicateOutput(adapter0/out0) hr=0x%08lx %s\n", hr, SUCCEEDED(hr) ? "OK" : "失败");
    } else printf("adapter0 无输出\n");
  }
  return 0;
}
