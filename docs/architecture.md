# projector 投屏器 — 架构方案 v0.1

> 目标：参考/逆向乐播投屏，实现 **Win11 PC 与 Android 手机 → 华为智慧屏 HD75QINA** 的局域网低延时投屏。
> 状态：**已确认（2026-11-06）**——手机端=iPhone；PC 端=C++20；接受 adb 侧载 APK；逆向=借鉴设计+自研 PJS。已转入 M0 环境搭建。

---

## 1. 目标与约束

| 项 | 内容 |
|---|---|
| 发送端 | Windows 11 PC（本机，走自研 PJS 协议）+ iPhone（电视端实现 **AirPlay 1 接收**，iPhone 用原生"屏幕镜像"直连、无需装 App） |
| 接收端 | 华为智慧屏 HD75QINA（Vision 3 系列 75" 4K，鸿蒙系统，可 adb 侧载 Android APK） |
| 网络 | 仅局域网（本机已确认：有线千兆，IP 10.5.9.11/24，**无 Wi-Fi 网卡**） |
| 延时 | 端到端目标 **<120ms**（有线理想 40–80ms glass-to-glass） |
| 画质 | 首选 1080p60（4K30 可选），H.264 硬编/硬解 |
| 声音 | 系统声音 + 麦克风（可选），AAC/Opus |

### 本机环境实测结论（决定架构路线的关键事实）
- **无 Wi-Fi 网卡** → Windows Miracast（Wi-Fi Direct）路线直接不可用；华为官方"电脑无线投屏"也正是 Miracast/华为分享路线，故排除，**必须自建接收端 APK 走局域网 IP**。
- GPU GTX 1050 Ti → **NVENC H.264 硬编码**可用；CPU i5-8500、16GB 内存，软编 1080p60 也可作 fallback。
- 开发工具链基本为空：仅有 git / node / python3.12 / dotnet 运行时（无 SDK）；**无 VS、无 JDK、无 Android SDK、无 cmake、无 Wireshark**；winget 可用。C 盘仅剩 ~10GB → **所有开发环境装到 D:\Dev**。

---

## 2. 对乐播投屏的逆向认知（M1 将进一步实测验证）

1. **拓扑**：发现（局域网 UDP 广播 + mDNS + 云端设备列表）→ 会话协商（TCP 控制通道，乐播 LBTP 常用 TCP 53555）→ 媒体流（H.264/AAC 走 TCP 或 RTP/UDP，局域网内直连）。
2. **TV 端形态**：Android APK 接收器（包名 `com.hpplay.happyplay.aw`，可 jadx 反编译），MediaCodec 硬解 + SurfaceView 直显 —— 与本项目路线完全一致。
3. **低延时手法**：无 B 帧、小 VBV、IDR 按需、解码直出 Surface、播放缓冲压到 1–2 帧。
4. **逆向工具箱**：jadx（反编译 TV/手机 APK 看协议字段与端口）、apktool + JDK、Wireshark + Npcap（真机抓包：手机乐播 → TV 接收端）、frida（可选，动态追踪混淆层）。
5. **风险**：LBTP 信令有混淆/加密版本，逐字节兼容官方客户端不保证 → 本项目"**借鉴其发现/会话/传输设计，协议自研**"（Q4）。

---

## 3. 总体架构

```
┌─────────────── LAN 10.5.9.0/24 ───────────────┐
│                                               │
│  sender-pc (Win11, PJS)     iPhone (原生镜像)   │
│  ┌─────────────────────┐   ┌───────────────┐  │
│  │ WGC/DXGI 采集        │   │ 控制中心"屏幕  │  │
│  │ MF/NVENC H.264 编码  │   │ 镜像"→ AirPlay │  │
│  │ WASAPI loopback 音频 │   │ (iPhone 免装App)│  │
│  │ 发现/会话/推流引擎    │   │ RTSP + AES     │  │
│  └───────┬─────────────┘   └──────┬────────┘  │
│          │ UDP 媒体 + TCP 控制      │ AirPlay   │
│          ▼                         ▼           │
│  receiver-tv (Android APK @ 华为智慧屏, 双入口) │
│  ┌─────────────────────────────────────────┐  │
│  │ 入口A: PJS (TCP 会话 + UDP 媒体)          │  │
│  │ 入口B: AirPlay 1 接收 (mDNS/HTTP/RTSP,   │  │
│  │   RSA-1024 换钥 + AES-127-CBC 解密)       │  │
│  │ 抖动缓冲→MediaCodec 低延时硬解→SurfaceView │  │
│  │ AudioTrack 低延时出声; NACK/IDR 请求      │  │
│  └─────────────────────────────────────────┘  │
└───────────────────────────────────────────────┘
```

三个可独立交付的组件 + 一份协议规范（先定协议再写代码，规范是唯一真源）：

| 组件 | 技术 | 说明 |
|---|---|---|
| `protocol/` | 规范文档 + 跨语言测试向量 | 发现/控制/媒体协议（§4），C++/Kotlin 共用同一测试向量 |
| `sender-pc` | **C++20**：Windows Graphics Capture + D3D11 + Media Foundation（NVENC 自动回退 x264）+ WASAPI；Win32 托盘 + 简洁窗口 | 低延时核心；建议栈见 Q2 |
| `airplay-rx`（receiver-tv 内模块） | Kotlin：AirPlay 1 接收（mDNS `_airplay._tcp`/`_raop._tcp`、HTTP pair/fp-device/fp-key、RTSP、RSA+AES-127-CBC），参照 libairplay/shairport-sync + iPhone 真机抓包逆向 | 服务 iPhone"屏幕镜像"（M4） |
| `receiver-tv` | Kotlin Android TV APK：minSdk 24 / target 34；MediaCodec + SurfaceView + AudioTrack | adb 网络调试安装（华为智慧屏开发者模式） |

## 4. 协议设计（PJS，借鉴 LBTP）

- **发现**：UDP 广播 :45688。接收端每 3s 发 beacon `{magic "PJS1", dev_id, name, tcp_port, ver}`；发送端可主动 probe。另实现 mDNS `_pjs._udp.local` 方便第三方发现。
- **控制**：TCP :53556，TLV 帧 + JSON 载荷：`HELLO → CAP/CAPREP → OPEN(分辨率/码率/编码能力) → READY → PLAY → HEARTBEAT(5s) → STAT(丢包/rtt) → TEARDOWN`。能力协商决定 codec/分辨率/码率，失败即降级 1080p30。
- **媒体**：UDP（端口会话内协商）。包头 16B：`{type(video/audio), seq, ts_us, flags{KEY,IDR,FIRST,LAST}, codec, len}`；H.264 Annex-B **一个 NAL slice 一包**，≤1200B 防 IP 分片；AAC-LC 48k 音频帧直填。
- **抗丢包（低延时优先）**：发送端保留最近 1s 环形缓冲；接收端 30ms 窗口 NACK 补传一次，仍丢→请求 IDR（关键帧 ~60ms 恢复画面），不做深度重传。
- **编码参数（延时核心）**：Constrained Baseline、**0 B 帧**、CBR + VBV ≤1 帧、GOP 2s、IDR 按需、WGC 抓取即编码。
- **音画同步**：发送端单调时钟 µs 时间戳 + 控制通道 NTP-lite RTT 对齐；**以音频为主时钟**，视频按 PTS 到点即显，缓冲深度 1–2 帧，漂移 >60ms 微调。
- **iPhone 入口（AirPlay 1）**：电视端以 mDNS 广播 `_airplay._tcp` / `_raop._tcp`，iPhone 控制中心"屏幕镜像"直连 → HTTP `/pair`、`/fp-device`、`/fp-key`（RSA-1024 换会话密钥）→ RTSP SETUP/RECORD 收 H.264（AES-127-CBC 加密流），音频走 RAOP 通道 + NTP 同步。握手细节闭源 → M1 用 iPhone 真机抓包逆向（对照 libairplay 等开源实现）。**延时预期需明确**：AirPlay 镜像延时由 iOS 侧缓冲决定，业界常见 0.3–1s（协议固有，电视端无法压到 PC 端水平）；若追求 <200ms，M6 自研 iOS 发送 App 走 PJS。

### 延时预算（有线千兆实测预期）

| 环节 | 预算 |
|---|---|
| WGC 采集 | ~16ms（1 帧 @60fps） |
| NVENC 编码（低延时档） | 5–10ms |
| LAN 传输+排队 | 1–3ms |
| MediaCodec 解码 | 5–15ms |
| Surface 合成显示 | 5–16ms |
| **合计** | **35–60ms**（目标 <120ms，优于乐播常见 100–200ms） |

测量方法：手机 240fps 慢动作拍摄"鼠标实体 vs 屏上光标"，或屏上 µs 计时器对照。

## 5. 关键设计决策与理由

1. **自建 APK 而非 Miracast/DLNA/AirPlay**：本机无 Wi-Fi，Miracast 不可用；DLNA 只能推视频不能镜像；AirPlay 华为不原生支持且接收端要做 mDNS+双栈+FairPlay 解码链，工作量更大（列为 M6 可选扩展）。
2. **H.264 而非 H.265**：NVENC/MediaCodec/兼容性三边最稳；1080p60 千兆 LAN 带宽（8–20Mbps）绰绰有余。4K 需求出现时再评估 HEVC。
3. **UDP + 轻量 NACK**，不用 RTSP/RTP 全家桶：协议面小、两端各 ~1k 行 C++/Kotlin 可控实现，利于低延时调优。
4. **PC 端选 C++**：WGC/MF/WASAPI 均为原生 API，无 interop 税；若你更看重开发速度可改 C#/.NET8（Q2，代价是 MF 编码要走 interop 或封装 FFmpeg）。

## 6. 目录规划

```
D:\DSH-Workplace\XingLianCast\
  docs/architecture.md  reverse/protocol-notes.md + airplay-notes.md(逆向记录)
  protocol/pjs-spec.md  protocol/vectors/(跨语言测试向量)
  sender-pc/        (C++, CMake)
  sender-ios/       (可选 M6，Swift，走 PJS 低延时)
  receiver-tv/      (Gradle, Kotlin, Android TV APK)
  tools/            (延时测量脚本、抓包过滤、联调脚本)
```

## 7. 里程碑

| 里程碑 | 内容 | 验收 |
|---|---|---|
| M0 环境 | winget/JDK17/VS BuildTools/CMake/Android cmdline-tools/Wireshark/jadx 装到 D:\Dev；TV 开开发者模式 `adb connect 10.5.9.x:5555` | 工具链自检通过、TV adb 可见 |
| M1 逆向 | jadx 反编译乐播 TV 端；真机抓包：乐播投屏全流程 → `protocol-notes.md`；iPhone→AirPlay 镜像 → `airplay-notes.md` | 端口/信令/媒体格式 + AirPlay 握手/加密结论成文 |
| M2 最小链路 | PC 1080p30 NVENC→UDP→TV 解码显示（无声） | 画面出、实测延时 <120ms |
| M3 完整 PC 投屏 | 60fps + 声音 + NACK/IDR + 掉线重连 + 码率自适应 | 拖动窗口/播放视频无明显音画不同步 |
| M4 iPhone 镜像 | TV 端 AirPlay 1 接收模块（抓包逆向 mDNS/HTTP/RTSP/加密链） | iPhone 控制中心"屏幕镜像"可直投，记录实测延时（AirPlay 协议固有约 0.3–1s） |
| M5 体验 | 设备发现列表、连接管理 UI、TV 端首页入口 | 三步内完成投屏 |
| M6 可选 | 自研 iOS 发送 App（走 PJS，预期 100–200ms）、Android 发送端、4K/HEVC | — |

## 8. M0 环境搭建清单（确认后执行）

全部装到 **D:\Dev**（C 盘仅剩 ~10GB）：

1. `winget install Microsoft.VisualStudio.2022.BuildTools`（VCTools 工作负载，`--installPath D:\Dev\VS2022BuildTools`）
2. `winget install Kitware.CMake --location D:\Dev\cmake`；NASM 可选
3. `winget install Microsoft.OpenJDK.17`（Android Gradle 需要）
4. Android：官方 `commandlinetools-win` → `D:\Dev\Android\Sdk`；`sdkmanager` 拉 `platform-tools / platforms;android-34 / build-tools;34.0.0`（Gradle 用 wrapper，**暂不装 Android Studio**，省 ~4GB；M4 需要 GUI 调试再装）
5. `winget install WiresharkWireshark.Wireshark`（含 Npcap）+ jadx 便携版
6. TV：设置→关于→版本号连点开启开发者模式 → 局域网 adb `adb connect <TV_IP>:5555`，先装一个 Hello APK 验证侧载可用（**M0 的第一验收项，风险前置**）

## 9. 已确认决策（2026-11-06）

- **Q1 手机端 = iPhone** → 电视端做 AirPlay 1 接收，iPhone 原生"屏幕镜像"直连、免装 App；如需 <200ms 低延时，M6 自研 iOS App 走 PJS 兜底
- **Q2 PC 发送端 = C++20**（WGC + Media Foundation NVENC + WASAPI）
- **Q3 接受 adb 侧载接收 APK**（M0 先装 Hello APK 验证智慧屏侧载，风险前置）
- **Q4 借鉴乐播设计 + 自研 PJS 协议**

## 10. 参考资料

- 华为官方：[Windows 电脑如何无线投屏至华为智慧屏](https://consumer.huawei.com/cn/support/content/zh-cn15783022/)、[哪些 Windows 电脑支持](https://consumer.huawei.com/cn/support/content/zh-cn15814471/)（Miracast 路线，兼容性受限）
- [HD75QINA = Vision 智慧屏 3 系列 75" 4K](https://wiki.smzdm.com/p/prvw53e/)
- [华为智慧屏 ADB 安装第三方软件教程](https://news.znds.com/article/69751.html)、[第三方应用安装失败官方说明](https://consumer.huawei.com/cn/support/content/zh-cn15773624/)
- 乐播 TV 端 APK 讨论：[52pojie lb投屏TV版](https://www.52pojie.cn/forum.php?mod=viewthread&tid=2077509)、[LBTP 反向控制信令解析](https://my.oschina.net/emacs_7988203/blog/19466981)
