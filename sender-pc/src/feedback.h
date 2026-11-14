// 接收端（TV）状态回传：UDP 45692。
// TV 每秒发一包文本（PJS/F1 + key=value 行），PC 侧解析后供界面显示。
// 同时负责发 ping（CTRL 文本 "p<ts_us>"），TV 原样回 "q<ts_us>"，用于测网络 RTT。
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <map>

namespace fb {

struct Snapshot {
  bool  alive   = false;   // 最近 3 秒内收到过回传
  float fps     = 0;
  long  lost    = 0;
  long  drop    = 0;
  long  idr     = 0;
  long  aud     = 0;
  long  adrop   = 0;
  int   w = 0, h = 0;
  double rttMs  = -1;      // 网络往返（ping/pong）
};

class Feedback {
 public:
  bool Start(int port = 45692) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    s_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_ == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = INADDR_ANY;
    if (::bind(s_, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) { closesocket(s_); s_ = INVALID_SOCKET; return false; }
    DWORD to = 200;
    setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    run_ = true;
    thr_ = std::thread([this] { Loop(); });
    return true;
  }

  void Stop() {
    run_ = false;
    if (thr_.joinable()) thr_.join();
    if (s_ != INVALID_SOCKET) { closesocket(s_); s_ = INVALID_SOCKET; }
  }
  ~Feedback() { Stop(); }

  // ping 走视频通道（pjs::Out::send_ping），这里只负责收 pong 并算 RTT
  void Tick(uint64_t /*now_ms*/) { }

  Snapshot Get() {
    std::lock_guard<std::mutex> lk(mu_);
    Snapshot s = snap_;
    if (hasRx_ && std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - lastRx_).count() > 3000)
      s.alive = false;
    return s;
  }

 private:
  void Loop() {
    char buf[1024];
    while (run_) {
      sockaddr_in from{};
      int fl = sizeof(from);
      int n = (int)::recvfrom(s_, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fl);
      if (n <= 0) continue;
      buf[n] = 0;
      std::string ip = inet_ntoa(from.sin_addr);
      {
        std::lock_guard<std::mutex> lk(mu_);
        target_ = ip;
      }
      std::string text(buf, n);
      // pong: "q<ms>"
      if (text.size() > 1 && text[0] == 'q') {
        unsigned long long t = strtoull(text.c_str() + 1, nullptr, 10);
        uint64_t now = NowMs();
        std::lock_guard<std::mutex> lk(mu_);
        if (t <= now) snap_.rttMs = (double)(now - t);
        lastRx_ = std::chrono::steady_clock::now();
        hasRx_ = true;
        snap_.alive = true;
        continue;
      }
      if (text.rfind("PJS/F1", 0) != 0) continue;
      std::lock_guard<std::mutex> lk(mu_);
      snap_.alive = true;
      lastRx_ = std::chrono::steady_clock::now();
      hasRx_ = true;
      size_t pos = 0;
      while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "fps") snap_.fps = (float)atof(v.c_str());
        else if (k == "lost") snap_.lost = atol(v.c_str());
        else if (k == "drop") snap_.drop = atol(v.c_str());
        else if (k == "idr") snap_.idr = atol(v.c_str());
        else if (k == "aud") snap_.aud = atol(v.c_str());
        else if (k == "adrop") snap_.adrop = atol(v.c_str());
        else if (k == "res") {
          int w = 0, h = 0;
          if (sscanf(v.c_str(), "%dx%d", &w, &h) == 2) { snap_.w = w; snap_.h = h; }
        }
      }
    }
  }

  static uint64_t NowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  SOCKET s_ = INVALID_SOCKET;
  std::thread thr_;
  std::atomic<bool> run_{false};
  std::mutex mu_;
  Snapshot snap_;
  std::string target_;
  std::chrono::steady_clock::time_point lastRx_;
  bool hasRx_ = false;
  uint64_t lastPing_ = 0;
};

} // namespace fb
