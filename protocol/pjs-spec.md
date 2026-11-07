# PJS 协议规范 v0.2（自研，借鉴乐播设计）

状态：**媒体层已实现并回环实测通过**（sender-pc ↔ 抓包验证 2026-11-07）；discovery/control 为设计稿（M3 实现）。

## 1. 通道总览

| 通道 | 传输 | 端口 | 状态 |
|---|---|---|---|
| 发现 discovery | UDP 广播/单播 | :45688 | 设计稿（M3） |
| 控制 control | TCP | :53556 | 设计稿（M3，NACK/IDR 请求/能力） |
| 媒体 media | UDP | :45690 | **已实现**（sender C++ / receiver Java） |

## 2. 发现（设计稿）

发送端每 2s UDP 广播 `PJSD` announce；接收端单播 `PJSQ` query，发送端单播应答。载荷含：设备名、codec 位图、媒体端口、会话 nonce。

## 3. 媒体层（已实现）

### 3.1 报文头（16 字节，大端）

```
偏移  长度  字段
0     2     magic  = 0x50 0x4A  ("PJ")
2     1     type   : 0=VIDEO, 1=AUDIO, 2=META
3     1     codec  : 0=H.264, 1=AAC (AUDIO 用)
4     4     seq    : 自增序号（video/meta 各自独立计数，从 1 起）
8     4     ts_lo  : 时间戳低 32 位（微秒，QPC 基准，仅用于相对同步）
12    1     flags  : bit0 FIRST | bit1 LAST | bit2 IDR | bit3 KEY | bit4 RETR
13    1     kind   : 0=SLICE, 2=META（CFG=1 预留）
14    2     payload_len
```

### 3.2 视频（type=0, kind=0）

- 一个访问单元(AU) = 一帧 Annex-B NAL 序列（AUD 起始）。
- **一包一 NALU**；NAL 超过 1200B 时切 ≤1200B 分片，首片置 FIRST、末片置 LAST（每个 NAL 是独立 FIRST|LAST 微组，接收端按 NAL 级重组，无需跨 NAL 状态）。
- AU 首 NAL 所在包置 FIRST，末 NAL 所在包置 LAST（与 NAL 微组共用 flag 位；NAL 切片场景以 NAL 级为准）。
- IDR 帧所有包置 IDR|KEY；普通 P 帧不置位。
- 实测样例：首包 payload `00000001 0910…`（AUD NAL type 9），MS 软编输出含 AUD/SPS/PPS/IDR。
- 跳过 NAL type > 23（保留 1–23 传输）。

### 3.3 meta（type=2, kind=2，每 2s 发送）

payload = `[ts_hi u16 大端][SPS+PPS Annex-B 拼接]`，>1200B 分片（FIRST/LAST 标记，seq 独立计数）。

- 作用：接收端周期性重配解码器（SPS/PPS 冗余下发）。`ts_hi = (ts_us>>16)&0xFFFF` 目前仅作校验参考；时序恢复以 ts_lo + 32 位回绕窗口（周期 71.6 分钟）为准。
- 实测样例：`a5cf | 00000001 674d…`（ts_hi=u16, 4 字节起始码 SPS nal type 7）。

### 3.4 时间戳

QPC 微秒（主机启动起算），仅用于帧间相对时序与音画同步；不要求跨机绝对对齐。

## 4. 控制（设计稿，M3）

TCP :53556。JSON line 或二进制 KV：HELLO/HELLO_ACK（能力协商）、NACK(seq 区间，丢包重传走 UDP RETR 或 TCP 旁路)、REQUEST_IDR、STATS。M2 无重传：丢包表现为画面马赛克，2s 后 meta+周期 IDR 自愈（当前仅首帧 IDR，M3 加周期 IDR）。

## 5. 兼容性

与乐播 HappyCast 报文不字节兼容（自研精简版）；语义映射见 docs/reverse/protocol-notes.md（M1 逆向产出）。
