// PJS 媒体层 (protocol/pjs-spec.md §3) — 16B 头, 大端
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <functional>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace pjs {
enum Type : uint8_t { VIDEO = 0, AUDIO = 1, PKT_META = 2 };
enum Flags : uint8_t { FIRST = 1, LAST = 2, IDR = 4, KEY = 8, RETR = 16 };
enum Kind : uint8_t { SLICE = 0, CFG = 1, KIND_META = 2 };
constexpr size_t MAX_PAYLOAD = 1200;
constexpr int DEFAULT_PORT = 45690;

inline void put8 (uint8_t* p, uint8_t v)  { *p = v; }
inline void put16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
inline void put32(uint8_t* p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
inline uint16_t get16(const uint8_t* p)   { return (uint16_t)(p[0] << 8 | p[1]); }
inline uint32_t get32(const uint8_t* p)   { return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3]; }

// 16B header: magic 'PJ' | type | codec | seq | ts_lo | flags | kind | payload_len
inline std::vector<uint8_t> packet(uint8_t type, uint8_t codec, uint32_t seq, uint64_t ts_us,
                                   uint8_t flags, uint8_t kind, const uint8_t* pl, size_t n) {
  std::vector<uint8_t> b(16 + n);
  put16(b.data(), (uint16_t)0x504A); b[2] = type; b[3] = codec;
  put32(b.data() + 4, seq); put32(b.data() + 8, (uint32_t)(ts_us & 0xFFFFFFFFu));
  b[12] = flags; b[13] = kind; put16(b.data() + 14, (uint16_t)n);
  memcpy(b.data() + 16, pl, n);
  return b;
}

// ---------- Annex-B NAL 工具 ----------
struct NalSpan { size_t off, len; uint8_t type; };
// 返回各 NAL 的 [起点(含起始码)..下一起始码)。输入为单个访问单元(Annex-B)。
inline std::vector<NalSpan> split_nals(const uint8_t* p, size_t n) {
  std::vector<NalSpan> out; size_t i = 0, sc = 0;
  auto find_sc = [&](size_t from, size_t& s_off, size_t& s_len) -> bool {
    for (size_t k = from; k + 2 < n; k++)
      if (p[k] == 0 && p[k+1] == 0 && (p[k+2] == 1 || (k + 3 < n && p[k+2] == 0 && p[k+3] == 1))) {
        size_t l = (p[k+2] == 1) ? 3 : 4;
        if (k + l < n) { s_off = k; s_len = l; return true; }
      }
    return false;
  };
  size_t off, len;
  if (!find_sc(0, off, len)) return out;
  while (true) {
    size_t body = off + len, next_off, next_len;
    NalSpan sp{ off, (find_sc(body, next_off, next_len) ? next_off : n) - off, uint8_t(p[body] & 0x1F) };
    out.push_back(sp);
    if (sp.off + sp.len >= n) break;
    off = out.back().off + out.back().len;
  }
  return out;
}
inline bool is_key_au(const uint8_t* p, size_t n) {
  for (auto& s : split_nals(p, n)) if (s.type == 5 || s.type == 7) return true;
  return false;
}
// 提取 SPS/PPS（含起始码）拼接
inline std::vector<uint8_t> extract_cfg(const uint8_t* p, size_t n) {
  std::vector<uint8_t> out;
  for (auto& s : split_nals(p, n))
    if (s.type == 7 || s.type == 8) { out.insert(out.end(), p + s.off, p + s.off + s.len); break; }
  for (auto& s : split_nals(p, n))
    if (s.type == 8) { out.insert(out.end(), p + s.off, p + s.off + s.len); break; }
  return out;
}

// ---------- UDP 发送 ----------
class UdpOut {
 public:
  bool open(const char* ip, int port) {
    WSADATA w; if (WSAStartup(0x202, &w)) return false;
    s_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&to_, 0, sizeof(to_)); to_.sin_family = AF_INET; to_.sin_port = htons((u_short)port);
    inet_pton(AF_INET, ip, &to_.sin_addr); return s_ != INVALID_SOCKET;
  }
  void send_raw(const std::vector<uint8_t>& b) {
    ::sendto(s_, (const char*)b.data(), (int)b.size(), 0, (sockaddr*)&to_, sizeof(to_));
  }
  // 访问单元 → 一包一 NAL；超长 NAL 切 ≤1200B 片。AU 首 NAL 置 FIRST、末 NAL 置 LAST。
  void send_au(const uint8_t* au, size_t n, uint64_t ts_us, bool key, bool idr) {
    auto nals = split_nals(au, n);
    std::vector<NalSpan*> seqn;
    for (auto& s : nals) if (s.type <= 23) seqn.push_back(&s);   // 跳过 AUD/SEI 之外的非标? 保留 1-23
    for (size_t i = 0; i < seqn.size(); i++) {
      auto& s = *seqn[i];
      bool first = i == 0, last = i + 1 == seqn.size();
      uint8_t base = (first ? FIRST : 0) | (last ? LAST : 0) | (key ? KEY : 0) | (idr ? IDR : 0);
      uint8_t f = (uint8_t)(au[s.off] & 0xE0); // NAL 头参考(分片时保留高3bit)
      (void)f;
      for (size_t off = 0; off < s.len; ) {
        size_t chunk = s.len - off > MAX_PAYLOAD ? MAX_PAYLOAD : s.len - off;
        bool cfirst = off == 0, clast = off + chunk >= s.len;
        uint8_t fl = base; if (seqn.size() > 1 || chunk < s.len) { fl = (base & ~uint8_t(FIRST)) | (cfirst ? FIRST : 0) | (clast ? LAST : 0); }
        send_raw(packet(VIDEO, 0, ++vseq_, ts_us, fl, SLICE, au + s.off + off, chunk));
        off += chunk;
      }
    }
  }
  // meta: [ts_hi u16][SPS+PPS Annex-B]，用于 ts 高位恢复与解码器重配
  void send_meta(const std::vector<uint8_t>& cfg, uint64_t ts_us) {
    if (cfg.empty()) return;
    std::vector<uint8_t> pl(2 + cfg.size()); put16(pl.data(), (uint16_t)(ts_us >> 16));
    memcpy(pl.data() + 2, cfg.data(), cfg.size());
    for (size_t off = 0; off < pl.size(); ) {
      size_t chunk = pl.size() - off > MAX_PAYLOAD ? MAX_PAYLOAD : pl.size() - off;
      bool cfirst = off == 0, clast = off + chunk >= pl.size();
      send_raw(packet(PKT_META, 0, ++mseq_, ts_us, (cfirst?FIRST:0)|(clast?LAST:0), KIND_META, pl.data() + off, chunk));
      off += chunk;
    }
  }
  // 音频: 一包一 ADTS AAC 帧 (spec §3: type=AUDIO codec=1)。仅音频线程调用。
  void send_audio(const uint8_t* f, size_t n, uint64_t ts_us) {
    send_raw(packet(AUDIO, 1, ++aseq_, ts_us, FIRST | LAST, SLICE, f, n));
  }
  uint64_t packets() const { return vseq_ + mseq_ + aseq_; }
  uint32_t audio_packets() const { return aseq_; }
 private:
  SOCKET s_ = INVALID_SOCKET; sockaddr_in to_{}; uint32_t vseq_ = 0, mseq_ = 0, aseq_ = 0;
};
} // namespace pjs
