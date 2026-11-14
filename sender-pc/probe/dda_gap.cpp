// Minimal DDA present-cadence probe: measures gaps between desktop frame present
// times with NO encoding/copy work, to tell whether desktop stalls are inherent
// to DWM/composition or caused by our capture pipeline.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <vector>
#include <algorithm>
using Microsoft::WRL::ComPtr;

int main() {
  SetConsoleOutputCP(65001);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
  D3D_FEATURE_LEVEL fl;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &dev, &fl, &ctx);
  if (FAILED(hr)) { printf("d3d fail 0x%08lx\n", hr); return 1; }
  ComPtr<IDXGIDevice> dxgi; dev.As(&dxgi);
  ComPtr<IDXGIAdapter> ad; dxgi->GetAdapter(&ad);
  ComPtr<IDXGIOutput> out;
  if (FAILED(ad->EnumOutputs(0, &out))) { printf("no output\n"); return 2; }
  ComPtr<IDXGIOutput1> out1; out.As(&out1);
  ComPtr<IDXGIOutputDuplication> dup;
  hr = out1->DuplicateOutput(dev.Get(), &dup);
  if (FAILED(hr)) { printf("dup fail 0x%08lx\n", hr); return 3; }
  printf("probe: sampling DDA present cadence (no encode)\n");
  LONGLONG last = 0;
  uint64_t end = GetTickCount64() + 30000;
  uint64_t sec_start = GetTickCount64(), sec_n = 0, sec_gmax = 0, sec_big = 0;
  while (GetTickCount64() < end) {
    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource> res;
    HRESULT a = dup->AcquireNextFrame(16, &fi, &res);
    if (a == DXGI_ERROR_WAIT_TIMEOUT) continue;
    if (FAILED(a)) { printf("acquire fail 0x%08lx\n", a); break; }
    LONGLONG pt = fi.LastPresentTime.QuadPart;
    if (last && pt > last) {
      uint64_t d = (uint64_t)(pt - last);           // 100ns units
      if (d > 2000000) d = 2000000;
      if (d > sec_gmax) sec_gmax = d;
      if (d > 250000) sec_big++;
      sec_n++;
    }
    last = pt;
    dup->ReleaseFrame();
    uint64_t now = GetTickCount64();
    if (now - sec_start >= 1000) {
      printf("fps=%.1f 间隔max=%.0fms 大间隔=%llu\n",
             sec_n / ((now - sec_start) / 1000.0), sec_gmax / 10000.0,
             (unsigned long long)sec_big);
      sec_start = now; sec_n = 0; sec_gmax = 0; sec_big = 0;
    }
  }
  return 0;
}
