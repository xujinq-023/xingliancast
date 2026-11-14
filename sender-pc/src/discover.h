// LAN discovery: listen for the TV receiver's UDP 45691 beacon.
// Payload (UTF-8, line oriented, extra keys tolerated):
//   PJS/1
//   name=QINL-370A
//   ip=10.5.9.22
//   port=45690
//   mod=QINL-370A
// Windows Firewall blocks unsolicited inbound UDP by default: the installer must add
// an inbound allow rule for UDP 45691 (or the user approves the prompt once).
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace pjs {

struct Device {
  std::string name;   // friendly name from the TV
  std::string ip;
  int port = 45690;
  std::string model;
};

inline int DiscoverPort() { return 45691; }

// Listen up to `ms` milliseconds and return every distinct device seen.
inline std::vector<Device> Discover(int ms) {
  std::vector<Device> out;
  WSADATA w{};
  if (WSAStartup(0x202, &w) != 0) return out;   // 可能在 UdpOut::open 之前被调用
  SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return out;
  int on = 1;
  ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons((u_short)DiscoverPort());
  a.sin_addr.s_addr = INADDR_ANY;
  if (::bind(s, (sockaddr*)&a, sizeof a) != 0) { ::closesocket(s); return out; }
  u_long nb = 1;
  ::ioctlsocket(s, FIONBIO, &nb);

  const DWORD t0 = GetTickCount();
  char buf[1024];
  while (GetTickCount() - t0 < (DWORD)ms) {
    sockaddr_in from{};
    int fl = sizeof from;
    int n = ::recvfrom(s, buf, sizeof buf - 1, 0, (sockaddr*)&from, &fl);
    if (n <= 0) { ::Sleep(20); continue; }
    buf[n] = 0;
    std::string text(buf, (size_t)n);
    if (text.rfind("PJS/1", 0) != 0) continue;      // not ours

    std::map<std::string, std::string> kv;
    size_t pos = 0;
    while (pos < text.size()) {
      size_t eol = text.find('\n', pos);
      if (eol == std::string::npos) eol = text.size();
      std::string line = text.substr(pos, eol - pos);
      size_t eq = line.find('=');
      if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
      pos = eol + 1;
    }
    Device d;
    d.name = kv.count("name") ? kv["name"] : "TV";
    d.model = kv.count("mod") ? kv["mod"] : "";
    d.ip = kv.count("ip") ? kv["ip"] : std::string(::inet_ntoa(from.sin_addr));
    if (kv.count("port")) d.port = atoi(kv["port"].c_str());
    if (d.ip.empty() || d.ip == "0.0.0.0") d.ip = ::inet_ntoa(from.sin_addr);
    bool dup = false;
    for (auto& e : out) if (e.ip == d.ip) { dup = true; break; }
    if (!dup) out.push_back(d);
  }
  ::closesocket(s);
  return out;
}

} // namespace pjs
