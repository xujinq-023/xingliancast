#include <windows.h>
#include <cstdio>
#include <string>
static void MkDirs(const std::string& dir) {
  if (dir.empty()) return;
  std::string cur;
  for (size_t i = 0; i < dir.size(); i++) {
    cur += dir[i];
    if (dir[i] == '\\' || dir[i] == '/') {
      if (cur.size() > 3) { BOOL ok = CreateDirectoryA(cur.c_str(), nullptr); DWORD e = ok ? 0 : GetLastError(); printf("mkdir [%s] ok=%d err=%lu\n", cur.c_str(), (int)ok, e); }
    }
  }
  BOOL ok = CreateDirectoryA(dir.c_str(), nullptr); DWORD e = ok ? 0 : GetLastError();
  printf("mkdir [%s] ok=%d err=%lu\n", dir.c_str(), (int)ok, e);
}
int main() {
  MkDirs("D:\\DSH-Workplace\\projector\\sender-pc\\build\\testdir\\a\\b");
  return 0;
}
