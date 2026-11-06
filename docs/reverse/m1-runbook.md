# M1 逆向作战手册（乐播投屏 + AirPlay）

目标：为 `protocol-notes.md`（借鉴 LBTP 设计）和 `airplay-notes.md`（iPhone 镜像兼容）产出实测结论。协议已定稿自研 PJS，逆向结论用于校勘设计，不追求逐字节兼容。

## A. 乐播投屏（LBTP）

### 材料
- 乐播 TV 接收端 APK（`com.hpplay.happyplay.aw`）：从智慧屏已安装版本 `adb pull /data/app/.../base.apk`，或 [52pojie TV 版讨论帖](https://www.52pojie.cn/forum.php?mod=viewthread&tid=2077509)
- 乐播投屏 PC 版 / 手机 App

### 步骤
1. **反编译**（静态）
   ```
   java -jar D:\Dev\tools\jadx\lib\jadx-1.5.0-all.jar --no-res --show-bad-code \
        -d D:\Dev\re\lbtp\src <apk>
   ```
   关注：`com.hpplay.` 包内的 socket 端口常量（53555?）、UDP 发现、信令类（`CommandInfo`/`MsgInfo`）、加解密入口。
2. **抓包**（动态）
   - PC 装 Wireshark+Npcap，本机有线网口直接抓（无需镜像）。
   - 场景 A：手机乐播 → 智慧屏乐播接收端（若电视已装）：过滤 `ip.addr==<手机IP>`；
   - 场景 B：PC 乐播客户端 → 接收端：过滤 `tcp.port==53555 || udp`。
   - 导出 pcap 存 `docs/reverse/`，记录：端口、发现报文（广播/mDNS 服务名）、TCP 首包握手字节（魔数/版本/JSON?）、媒体走 TCP 还是 UDP、RTP 封装与否。
3. **对照输出** `docs/reverse/protocol-notes.md`：
   - 发现机制表 / 信令时序图 / 媒体封装 / 延时手法（B帧、缓冲深度可由 RTSP/时间戳观察）
   - 结论标注"已实测/仅静态推断"。

## B. AirPlay 1（iPhone 屏幕镜像，服务 M4）

### 材料
- 参考开源：libairplay（RPi 播放器内）、shairport-sync（音频 RAOP）、openairplay 文档站、`mzgregor/mediareceiver`
- 对照接收端：一台支持 AirPlay 的设备/软件（如 Mac 的 AirPlay 接收、或市场 AirPlay 播放器 App）抓"iPhone → 它"的流量

### 步骤
1. Wireshark 过滤 `dns.qry.name contains "airplay" || dns.qry.name contains "raop"`：记录 mDNS 服务 TXT 字段（`deviceid`/`features`/`model`/`name`）。
2. iPhone 发起"屏幕镜像"全过程抓包：HTTP `POST /pair`、`/fp-device`、`/fp-key`（RSA-1024 换 AES 会话密钥）、RTSP `SETUP/RECORD`（视频 stream 端口对）、AES-127-CBC 加密的 H.264 流参数（iv2/eaKey 由 ekey 提供）、RAOP 音频（TCP:PORT + NTP 端口同步）。
3. 输出 `docs/reverse/airplay-notes.md`：接收端最小实现清单（mDNS 广播字段 → HTTP 处理器 → RTSP 状态机 → 解密 → MediaCodec 直通）。

## C. 验收
- 两份 notes 成文；PJS spec 若有因逆向结论引发的修改，走 spec 修订记录并 commit。
