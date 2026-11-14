// File logger for the sender: timestamped lines, size-based rotation, crash dumps.
//
// - All output goes through Logf(). When a console is attached the line is also
//   printed there, so running from cmd keeps the old behaviour.
// - The capture loop must never block on disk I/O: Logf() only enqueues, a worker
//   thread does the writes (measured: an unbuffered stdout write per second cost
//   30-70ms stalls in the capture loop).
// - Rotation: when the current file exceeds maxBytes it becomes "<name>.1" (the
//   previous .1 is overwritten). Two files, so disk use is bounded by 2*maxBytes.
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

namespace logx {

inline std::mutex            g_mu;
inline std::deque<std::string> g_q;
inline std::atomic<bool>     g_run{ false };
inline std::thread           g_th;
inline FILE*                 g_fp = nullptr;
inline std::string           g_path;
inline size_t                g_max = 5u << 20;
inline size_t                g_written = 0;
inline bool                  g_console = false;
inline DWORD                 g_openErr = 0;

inline void MkDirs(const std::string& dir) {
  if (dir.empty()) return;
  // 注意：CreateDirectory 用「带尾随反斜杠」的路径时，在 %LOCALAPPDATA% 这类目录下
  // 会返回 ERROR_ACCESS_DENIED(5)（实测），所以创建前必须去掉尾随分隔符。
  std::string cur;
  for (size_t i = 0; i < dir.size(); i++) {
    cur += dir[i];
    if (dir[i] == '\\' || dir[i] == '/') {
      std::string p = cur;
      while (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
      if (p.size() > 2) CreateDirectoryA(p.c_str(), nullptr);
    }
  }
  CreateDirectoryA(dir.c_str(), nullptr);
}
inline void Rotate() {
  if (g_fp) { fflush(g_fp); fclose(g_fp); g_fp = nullptr; }
  std::string old1 = g_path + ".1";
  DeleteFileA(old1.c_str());                       // 覆盖上一代
  MoveFileA(g_path.c_str(), old1.c_str());
  g_fp = fopen(g_path.c_str(), "wb");
  g_written = 0;
}

inline void WriteOne(const std::string& s) {
  if (g_console) {
    fwrite(s.data(), 1, s.size(), stdout);
    fflush(stdout);
  }
  if (!g_fp) return;
  if (g_written + s.size() > g_max) Rotate();
  if (g_fp) {
    fwrite(s.data(), 1, s.size(), g_fp);
    fflush(g_fp);
    g_written += s.size();
  }
}

inline void Worker() {
  for (;;) {
    std::string s;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      if (!g_q.empty()) { s = std::move(g_q.front()); g_q.pop_front(); }
    }
    if (!s.empty()) WriteOne(s);
    else {
      if (!g_run) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
  }
  if (g_fp) { fflush(g_fp); fclose(g_fp); g_fp = nullptr; }
}

// Default log path: %LOCALAPPDATA%\Projector\logs\sender.log
inline std::string DefaultPath() {
  char buf[MAX_PATH]{};
  DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
  std::string base = (n > 0 && n < MAX_PATH) ? std::string(buf) : std::string(".");
  return base + "\\Projector\\logs\\sender.log";
}

// 候选路径：某些环境（沙箱/企业策略）会拒绝在 %LOCALAPPDATA% 建目录（实测 ERROR_ACCESS_DENIED），
// 所以按优先级逐个尝试，第一个能打开的生效。
inline bool OpenAt(const std::string& p) {
  g_path = p;
  size_t cut = p.find_last_of("\\/");
  if (cut != std::string::npos) MkDirs(p.substr(0, cut));
  g_fp = fopen(p.c_str(), "ab");
  if (!g_fp) return false;
  fseek(g_fp, 0, SEEK_END);
  g_written = (size_t)ftell(g_fp);
  return true;
}

inline std::string ExeDir() {
  char buf[MAX_PATH]{};
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return ".";
  std::string p(buf, n);
  size_t cut = p.find_last_of("\\/");
  return cut == std::string::npos ? std::string(".") : p.substr(0, cut);
}

inline void Shutdown();   // 前置声明：Init 里的 atexit 需要
inline bool Init(const std::string& path, size_t maxBytes) {
  g_max = (maxBytes < (1u << 20)) ? (5u << 20) : maxBytes;
  // 总是写 stdout：GUI 用 CreateNoWindow + 重定向启动 sender 时没有控制台，
  // 只检测 GetConsoleWindow() 会让界面收不到任何输出（实测状态卡在「启动中」）。
  // 真正的 I/O 在后台线程，不在采集循环里，不会造成卡顿。
  g_console = true;
  g_run = true;
  g_th = std::thread(Worker);
  std::atexit([] { logx::Shutdown(); });   // 任何提前 return 也要 join，否则 std::terminate
  if (!path.empty()) return OpenAt(path);
  if (OpenAt(DefaultPath())) return true;
  char tmp[MAX_PATH]{};
  DWORD n = GetEnvironmentVariableA("TEMP", tmp, MAX_PATH);
  if (n > 0 && n < MAX_PATH &&
      OpenAt(std::string(tmp) + "\\Projector\\logs\\sender.log")) return true;
  return OpenAt(ExeDir() + "\\logs\\sender.log");
}
inline void Shutdown() {
  if (!g_run) return;
  g_run = false;
  if (g_th.joinable()) g_th.join();
}

inline void Put(int level, const char* fmt, va_list ap) {
  char msg[2048];
  vsnprintf(msg, sizeof msg, fmt, ap);
  SYSTEMTIME st;
  GetLocalTime(&st);
  static const char* lv[] = { "INFO", "WARN", "ERR " };
  char line[2200];
  int n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] %s\n",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                   st.wMilliseconds, lv[level & 3], msg);
  if (n <= 0) return;
  if ((size_t)n > sizeof line - 1) n = (int)sizeof line - 1;
  std::lock_guard<std::mutex> lk(g_mu);
  g_q.emplace_back(line, (size_t)n);
}

inline void Logf(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt); Put(0, fmt, ap); va_end(ap);
}
inline void Warnf(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt); Put(1, fmt, ap); va_end(ap);
}
inline void Errf(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt); Put(2, fmt, ap); va_end(ap);
}

// Crash dump: written synchronously (the queue may never drain after a crash).
inline LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::string p = g_path.empty() ? DefaultPath() : g_path;
  size_t cut = p.find_last_of("\\/");
  std::string dir = (cut == std::string::npos) ? std::string(".") : p.substr(0, cut);
  char name[MAX_PATH]{};
  snprintf(name, sizeof name, "%s\\crash-%04d%02d%02d-%02d%02d%02d.txt", dir.c_str(),
           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  FILE* f = fopen(name, "wb");
  if (f) {
    const char* code = "?";
    DWORD c = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    char tmp[512];
    snprintf(tmp, sizeof tmp,
             "Projector sender crash\n"
             "time: %04d-%02d-%02d %02d:%02d:%02d.%03d\n"
             "exception code: 0x%08lx\n"
             "exception address: %p\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds, (unsigned long)c,
             (void*)(ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr));
    fwrite(tmp, 1, strlen(tmp), f);
    fclose(f);
    (void)code;
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace logx

// 便于各头文件直接调用（encoder.h / audio.h 不带命名空间）
using logx::Logf;
using logx::Warnf;
using logx::Errf;
