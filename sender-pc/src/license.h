// 授权校验（原生实现）：ECDSA P-256 验签 + 机器码绑定 + 每日试用额度
// 放在引擎里，界面程序被改写也绕不过：验不过直接退出，不给推流。
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <cstdio>
#include <ctime>
#pragma comment(lib, "bcrypt.lib")

namespace lic {

// --- 公钥（与 XingLianCast.exe 中一致，X/Y 为 base64）---
static const char* kPubX = "2Z7kYMRV9Tt3n638nxJbIL5RZ54GRAIo2GkduUqAxK8=";
static const char* kPubY = "j5/5Bkmi/0B6g6n8N2PMwD6psGXzXxZiweDxDZztg+Y=";
static const int kPayloadLen = 12;
static const unsigned kNeverExpire = 0xFFFFFFFFu;
static const int kTrialSecondsPerDay = 300;

// ---------------- base64 ----------------
inline std::vector<uint8_t> b64(const std::string& s) {
  std::vector<uint8_t> out;
  int val = 0, bits = -8;
  for (char ch : s) {
    int c;
    if (ch >= 'A' && ch <= 'Z') c = ch - 'A';
    else if (ch >= 'a' && ch <= 'z') c = ch - 'a' + 26;
    else if (ch >= '0' && ch <= '9') c = ch - '0' + 52;
    else if (ch == '+' || ch == '-') c = 62;
    else if (ch == '/' || ch == '_') c = 63;
    else if (ch == '=') continue;
    else continue;
    val = (val << 6) | c;
    bits += 6;
    if (bits >= 0) { out.push_back((uint8_t)((val >> bits) & 0xFF)); bits -= 8; }
  }
  return out;
}

inline std::string b64url_of(std::vector<uint8_t> const&) { return ""; }

// ---------------- SHA256 ----------------
inline bool sha256(const void* data, size_t len, uint8_t out[32]) {
  BCRYPT_ALG_HANDLE h = nullptr;
  if (BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
  bool ok = false;
  BCRYPT_HASH_HANDLE hh = nullptr;
  if (BCryptCreateHash(h, &hh, nullptr, 0, nullptr, 0, 0) == 0) {
    if (BCryptHashData(hh, (PUCHAR)data, (ULONG)len, 0) == 0 &&
        BCryptFinishHash(hh, out, 32, 0) == 0) ok = true;
    BCryptDestroyHash(hh);
  }
  BCryptCloseAlgorithmProvider(h, 0);
  return ok;
}

// ---------------- 机器码（与 C# 完全一致）----------------
// 必须和界面 ProjectorUi.License.MachineCode()、发码工具 MakeLicense.LocalMachineCode()
// 逐字节一致，否则会出现「注册成功但仍不能投屏」。三处统一口径：
//   机器名(UTF-8) | 系统盘卷标(UTF-8，不截断) : 系统盘总容量(十进制) | 逻辑核数
// 注意：系统盘取 GetSystemDirectory 的盘符（不是硬编码 C:），卷标用宽字符 API 再转 UTF-8
// （中文卷标用 ANSI 会得到 GBK 字节，和 C# 的 UTF-8 对不上）。
inline std::wstring u8w(const std::string& s) {          // UTF-8 → UTF-16
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring out((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
  return out;
}
inline std::string w8u(const std::wstring& w) {          // UTF-16 → UTF-8
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string out((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
  return out;
}

inline std::string sys_drive_root() {          // 例 "C:\\"，与 C# Path.GetPathRoot(Environment.SystemDirectory) 一致
  wchar_t dir[MAX_PATH] = {};
  UINT n = GetSystemDirectoryW(dir, MAX_PATH);
  if (n >= 2) return w8u(std::wstring(dir, 2) + L"\\");
  return "C:\\";
}

inline std::string machine_raw() {
  wchar_t wname[256] = {};
  DWORD n = 255;
  GetComputerNameW(wname, &n);
  std::wstring prefix = std::wstring(wname, n);

  std::string drive = sys_drive_root();
  std::wstring wdrive = u8w(drive);

  wchar_t wvol[MAX_PATH] = {}, wfs[64] = {};
  DWORD serial = 0, maxComp = 0, flags = 0;
  GetVolumeInformationW(wdrive.c_str(), wvol, MAX_PATH, &serial, &maxComp, &flags, wfs, 64);

  ULARGE_INTEGER freeBytes{}, totalBytes{};
  GetDiskFreeSpaceExW(wdrive.c_str(), &freeBytes, &totalBytes, nullptr);

  SYSTEM_INFO si{};
  GetSystemInfo(&si);

  char b[1024];
  snprintf(b, sizeof b, "%s|%s:%llu|%u",
           w8u(prefix).c_str(), w8u(std::wstring(wvol)).c_str(),
           (unsigned long long)totalBytes.QuadPart, (unsigned)si.dwNumberOfProcessors);
  return b;
}

inline std::string machine_code() {
  std::string raw = machine_raw();
  uint8_t h[32];
  if (!sha256(raw.c_str(), raw.size(), h)) return "0000-0000-0000";
  char hex[13];
  for (int i = 0; i < 6; i++) snprintf(hex + i * 2, 3, "%02X", h[i]);
  char out[16];
  snprintf(out, sizeof out, "%.4s-%.4s-%.4s", hex, hex + 4, hex + 8);
  return out;
}

// 诊断用：返回机器码的原始拼接串（排查和界面对不上时用）
inline std::string machine_debug() { return machine_raw(); }

// ---------------- 存储位置（与界面同一套回退，宽字符路径）----------------
// 注意：目录名"星联投屏"用 UTF-8 字节序列在运行时转成 UTF-16，
// 避免源文件编码（GBK/UTF-8）影响中文路径。（u8w 定义在文件上部）
inline const std::wstring& app_folder() {
  static const std::wstring f = u8w("\xE6\x98\x9F\xE8\x81\x94\xE6\x8A\x95\xE5\xB1\x8F");  // 星联投屏
  return f;
}
inline std::vector<std::wstring> store_dirs() {
  std::vector<std::wstring> v;
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  if (n && n < MAX_PATH) v.push_back(std::wstring(buf) + L"\\" + app_folder());
  n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
  if (n && n < MAX_PATH) v.push_back(std::wstring(buf) + L"\\" + app_folder());
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir(exe);
  size_t p = dir.find_last_of(L"\\/");
  if (p != std::wstring::npos) v.push_back(dir.substr(0, p) + L"\\data");
  n = GetEnvironmentVariableW(L"TEMP", buf, MAX_PATH);
  if (n && n < MAX_PATH) v.push_back(std::wstring(buf) + L"\\" + app_folder());
  return v;
}

inline bool read_file_w(const std::wstring& path, std::string& out) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz > 0 && sz < 65536) {
    out.resize((size_t)sz);
    size_t rd = fread(&out[0], 1, (size_t)sz, f);
    out.resize(rd);
  }
  fclose(f);
  while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return !out.empty();
}

inline bool write_file_w(const std::wstring& path, const std::string& data) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
  size_t w = fwrite(data.data(), 1, data.size(), f);
  fflush(f);
  fclose(f);
  if (w != data.size()) return false;
  std::string back;
  return read_file_w(path, back) && back == data;
}

inline std::string today_str_for_file() {
  time_t t = time(nullptr);
  struct tm tmv{};
  localtime_s(&tmv, &t);
  char b[16];
  snprintf(b, sizeof b, "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  return b;
}
inline std::string store_read_all_best(const std::string& name, bool takeMaxForToday) {
  std::wstring wn = u8w(name);
  int best = -1;
  std::string today = today_str_for_file();
  for (auto& d : store_dirs()) {
    CreateDirectoryW(d.c_str(), nullptr);
    std::string s;
    if (!read_file_w(d + L"\\" + wn, s)) continue;
    if (!takeMaxForToday) return s;
    size_t bar = s.find('|');
    if (bar == std::string::npos) continue;
    if (s.substr(0, bar) != today) continue;
    int v = atoi(s.c_str() + bar + 1);
    if (v > best) best = v;
  }
  if (!takeMaxForToday) return "";
  if (best < 0) return "";
  char b[64];
  snprintf(b, sizeof b, "%s|%d", today.c_str(), best);
  return b;
}

inline std::string store_read(const std::string& name) {
  std::wstring wn = u8w(name);
  for (auto& d : store_dirs()) {
    CreateDirectoryW(d.c_str(), nullptr);
    std::string s;
    if (read_file_w(d + L"\\" + wn, s)) return s;
  }
  wchar_t v[2048] = {};
  DWORD cb = sizeof v, type = 0;
  std::wstring regName = u8w(name);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\XingLian\\Projector", regName.c_str(),
                   RRF_RT_REG_SZ, &type, v, &cb) == ERROR_SUCCESS) {
    int need = WideCharToMultiByte(CP_UTF8, 0, v, -1, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)(need > 0 ? need - 1 : 0), '\0');
    if (need > 1) WideCharToMultiByte(CP_UTF8, 0, v, -1, &out[0], need, nullptr, nullptr);
    return out;
  }
  return "";
}

inline bool store_write(const std::string& name, const std::string& data) {
  bool any = false;
  std::wstring wn = u8w(name);
  for (auto& d : store_dirs()) {
    CreateDirectoryW(d.c_str(), nullptr);
    if (write_file_w(d + L"\\" + wn, data)) any = true;
  }
  HKEY k = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\XingLian\\Projector", 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
    std::wstring wv = u8w(data);
    RegSetValueExW(k, wn.c_str(), 0, REG_SZ, (const BYTE*)wv.c_str(),
                   (DWORD)((wv.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    any = true;
  }
  return any;
}
// ---------------- 验签 ----------------
inline bool verify(const std::string& codeIn, std::string& why, bool& permanent) {
  permanent = false;
  std::string code = codeIn;
  while (code.size() >= 3 && (unsigned char)code[0] == 0xEF &&
         (unsigned char)code[1] == 0xBB && (unsigned char)code[2] == 0xBF)
    code.erase(0, 3);                       // 跳过 UTF-8 BOM
  if (code.size() < 8) { why = "no-license"; return false; }
  if (code.compare(0, 4, "XL1.") != 0) { why = "bad-prefix"; return false; }
  auto all = b64(code.substr(4));
  if (all.size() != (size_t)(kPayloadLen + 64)) { why = "bad-length"; return false; }

  uint8_t payload[kPayloadLen];
  memcpy(payload, all.data(), kPayloadLen);
  const uint8_t* sig = all.data() + kPayloadLen;

  // 机器码绑定
  std::string mc = machine_code();
  uint8_t mh[32];
  if (!sha256(mc.c_str(), mc.size(), mh)) { why = "hash-fail"; return false; }
  for (int i = 0; i < 6; i++) if (payload[1 + i] != mh[i]) { why = "machine-mismatch"; return false; }

  // 到期
  unsigned days = ((unsigned)payload[7] << 24) | ((unsigned)payload[8] << 16) |
                  ((unsigned)payload[9] << 8) | payload[10];
  if (days != kNeverExpire) {
    // 2026-01-01 起的天数
    SYSTEMTIME st{}; GetSystemTime(&st);
    FILETIME ft{}; SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u{}; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    ULONGLONG nowDays = (u.QuadPart - 116444736000000000ULL) / 864000000000ULL;   // 自 1970-01-01
    ULONGLONG epochDays = 20454ULL;                                               // 2026-01-01
    if (nowDays >= epochDays + days) { why = "expired"; return false; }   // 恰好 N 天，不多给一天
  } else permanent = true;

  if ((payload[11] & 1) == 0) { why = "no-feature"; return false; }

  // 公钥 blob：BCRYPT_ECCPUBLIC_BLOB
  auto x = b64(kPubX), y = b64(kPubY);
  if (x.size() != 32 || y.size() != 32) { why = "pubkey"; return false; }
  struct EccPub { BCRYPT_ECCKEY_BLOB hdr; uint8_t X[32]; uint8_t Y[32]; } blob{};
  blob.hdr.dwMagic = 0x31534345;   // 'ECS1'
  blob.hdr.cbKey = 32;
  memcpy(blob.X, x.data(), 32);
  memcpy(blob.Y, y.data(), 32);

  uint8_t hash[32];
  if (!sha256(payload, kPayloadLen, hash)) { why = "hash2-fail"; return false; }

  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) { why = "cn-provider"; return false; }
  BCRYPT_KEY_HANDLE key = nullptr;
  NTSTATUS st2 = BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                                     (PUCHAR)&blob, sizeof blob, 0);
  bool ok = false;
  if (st2 == 0) {
    st2 = BCryptVerifySignature(key, nullptr, hash, sizeof hash, (PUCHAR)sig, 64, 0);
    ok = (st2 == 0);
    BCryptDestroyKey(key);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  if (!ok) { why = "bad-signature"; return false; }
  why = "ok";
  return true;
}

// ---------------- 试用额度（引擎自己记，界面记的只用于显示）----------------
inline std::string today_str() {
  time_t t = time(nullptr);
  struct tm tmv{};
  localtime_s(&tmv, &t);
  char b[16];
  snprintf(b, sizeof b, "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  return b;
}

// 取所有位置（含注册表）里某个整数记录的最大值：用于"历史最大用量"，防止删文件清零
inline int store_read_max_int(const std::string& name) {
  std::wstring wn = u8w(name);
  int best = -1;
  for (auto& d : store_dirs()) {
    CreateDirectoryW(d.c_str(), nullptr);
    std::string s;
    if (!read_file_w(d + L"\\" + wn, s)) continue;
    int v = atoi(s.c_str());
    if (v > best) best = v;
  }
  wchar_t v2[64] = {};
  DWORD cb = sizeof v2, type = 0;
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\XingLian\\Projector", wn.c_str(),
                   RRF_RT_REG_SZ, &type, v2, &cb) == ERROR_SUCCESS) {
    int v = _wtoi(v2);
    if (v > best) best = v;
  }
  return best;
}

// 试用额度（H6 加固）：
//  - 记录"历史单日最大用量"：删掉今天的记录文件也不能清零
//  - 记录"最近一次见到的日期"：系统时间被往回拨时不重置额度
inline int trial_used() {
  std::string s = store_read_all_best("engine_trial.dat", true);
  int maxEver = store_read_max_int("engine_trial_max.dat");
  if (maxEver >= kTrialSecondsPerDay) return kTrialSecondsPerDay;   // 曾经用满过 → 直接拦
  std::string last = store_read("engine_trial_last.dat");
  std::string today = today_str();
  if (!last.empty() && last > today) return kTrialSecondsPerDay;     // 时钟回拨：按用满处理
  if (s.empty()) return 0;
  size_t bar = s.find('|');
  if (bar == std::string::npos) return 0;
  if (s.substr(0, bar) != today) return 0;
  int used = atoi(s.c_str() + bar + 1);
  return used > maxEver ? used : maxEver;
}

inline void trial_set(int used) {
  std::string today = today_str();
  char b[64];
  snprintf(b, sizeof b, "%s|%d", today.c_str(), used);
  store_write("engine_trial.dat", b);
  int maxEver = store_read_max_int("engine_trial_max.dat");
  if (used > maxEver) {
    char m[32];
    snprintf(m, sizeof m, "%d", used);
    store_write("engine_trial_max.dat", m);
  }
  // 只在日期前进时更新（回拨不改，保住"见过的最晚日期"）
  std::string last = store_read("engine_trial_last.dat");
  if (last.empty() || today > last) store_write("engine_trial_last.dat", today);
}

// 返回：0 = 允许；1 = 未授权且试用已用完
inline int check_start(std::string& msg, bool& licensed) {
  std::string code = store_read("license.dat");
  std::string why; bool perm = false;
  licensed = verify(code, why, perm);
  if (licensed) { msg = "已授权"; return 0; }
  int used = trial_used();
  if (used >= kTrialSecondsPerDay) {
    msg = "试用已用完";
    return 1;
  }
  msg = "试用中";
  return 0;
}

}  // namespace lic
