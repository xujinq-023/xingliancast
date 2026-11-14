// 枚举系统视频编码器 MFT：(a) NV12→H264+硬件 (b) NV12→H264+软件 (c) ALL 无类型过滤
// 打印每个候选激活后的 FriendlyName，定位 "硬件 H264 MFT 去哪了"。
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <cstdio>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

static void dump(const wchar_t* title, DWORD flags, MFT_REGISTER_TYPE_INFO* ti) {
  IMFActivate** acts = nullptr; UINT32 n = 0;
  HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, ti, ti, &acts, &n);
  wprintf(L"== %s: hr=0x%08lx  n=%u\n", title, hr, n);
  if (SUCCEEDED(hr) && acts) {
    for (UINT32 i = 0; i < n; i++) {
      WCHAR name[256] = L"?";
      ComPtr<IMFTransform> t;
      HRESULT ha = acts[i]->ActivateObject(IID_PPV_ARGS(&t));
      if (SUCCEEDED(ha)) {
        ComPtr<IMFAttributes> at; t->GetAttributes(&at);
        WCHAR* nm = nullptr; UINT32 len = 0;
        if (at && SUCCEEDED(at->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nm, &len)) && nm) {
          wcsncpy_s(name, nm, _TRUNCATE); CoTaskMemFree(nm);
        }
      } else swprintf_s(name, L"(activate-failed hr=0x%08lx)", ha);
      wprintf(L"  [%u] %ls\n", i, name);
    }
    for (UINT32 i = 0; i < n; i++) acts[i]->Release();
    CoTaskMemFree(acts);
  }
}

int main() {
  if (FAILED(MFStartup(MF_VERSION))) { printf("MFStartup fail\n"); return 1; }
  MFT_REGISTER_TYPE_INFO ti{ MFVideoFormat_NV12, MFVideoFormat_H264 };
  MFT_REGISTER_TYPE_INFO ti_in{ MFVideoFormat_NV12, GUID_NULL };
  dump(L"NV12-H264 HARDWARE", MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &ti);
  dump(L"NV12-H264 HW no-SORT", MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_HARDWARE, &ti);
  dump(L"NV12-in-only HW no-SORT", MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_HARDWARE, &ti_in);
  dump(L"NV12-H264 SOFT", MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &ti);
  dump(L"ALL (0x3F) ANY-TYPE", MFT_ENUM_FLAG_ALL, nullptr);
  MFShutdown();
  return 0;
}
