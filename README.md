# 星联投屏 · XingLianCast

Windows 电脑 → 智慧屏（Android TV）的**局域网投屏**方案，自研 PJS 协议，无需公网、无需账号。

- **电脑端**：`XingLianCast.exe`（界面）+ `XingLianCastSender.exe`（采集/编码引擎）
- **电视端**：`receiver-tv.apk`（原生 Android 接收端）
- **特点**：低延迟（同网段 ~150ms）、支持声音、锁屏自动遮罩保护隐私、断线自动恢复

---

## 功能

| 功能 | 说明 |
|---|---|
| 全屏镜像 | DXGI Desktop Capture，支持多显示器选择 |
| 硬件编码 | NVIDIA / Intel / AMD 硬件 MFT，失败自动降级到软件编码 |
| 声音传输 | WASAPI loopback → AAC 128k，可在投屏中随时开关 |
| 锁屏遮罩 | Win+L 后电视显示「电脑已锁屏」页（不泄露桌面内容），解锁自动恢复 |
| 断线恢复 | 网络波动 / 显示器拔出 / 引擎异常退出 都会自动重连 |
| 每日试用 | 未注册每天 5 分钟，注册后不限时 |
| 离线授权 | 激活码为 ECDSA 签名，绑机器码、不联网校验 |

---

## 架构

```
┌──────────────────── Windows PC ────────────────────┐        ┌──── 智慧屏 (Android) ────┐
│  XingLianCast.exe（WinForms 界面）                  │        │  receiver-tv.apk         │
│      │ 命令行 + 共享内存控制                        │        │                          │
│      ▼                                              │        │                          │
│  XingLianCastSender.exe                             │  PJS   │  RxEngine（Java）        │
│    DXGI 采集 → NV12 → H.264 MFT → 拆包 ─────────────┼───────▶│  解包 → MediaCodec → 显示 │
│    WASAPI → AAC → 拆包                              │ UDP    │  AudioTrack 播放          │
│      ▲                                              │ 45690  │      │                   │
│      │  反馈（fps/丢包/rtt）◀───────────────────────┼────────│  UDP 45692 回传           │
└────────────────────────────────────────────────────┘        └──────────────────────────┘
                    ▲  发现信标 UDP 45691（电视每秒广播）
```

协议细节见 [`docs/architecture.md`](docs/architecture.md)。

---

## 快速开始

### 1. 电视端

把 `receiver-tv.apk` 装到电视（U 盘或 `adb install`），打开后屏幕显示：

```
星联投屏
等待电脑投屏…
QINL-370A   10.5.9.22:45690
```

记下电视的 **IP**。

### 2. 电脑端

运行安装包 `XingLianCast-Setup-1.0.13.exe`（默认装到 `C:\Program Files\XingLianCast`），
首次启动在防火墙弹窗里选「允许访问」。

### 3. 投屏

选择显示器与码率 → 点「开始投屏」。电视通常 1~3 秒内出现画面。

详细操作见 [`docs/用户手册.md`](docs/用户手册.md)。

电视上各种提示（等待投屏 / 正在连接 / 电脑已锁屏 / 已停止投屏 / 连接已断开 / 另一台电脑被拒绝）
的含义与遥控器操作见 [`docs/电视端提示说明.md`](docs/电视端提示说明.md)。

---

## 授权与价格

| 项目 | 价格 |
|---|---|
| 试用 | 每天 5 分钟，免费 |
| 年费授权 | **10 元/年** |
| 终生授权 | **58 元（永久）** |

- 注册入口：软件右上角 **☰ → 正版注册**，把「本机机器码」发给我们换取激活码
- 联系方式：**xujinq@hotmail.com** / **+86 13996351705**
- 授权与电脑绑定（换电脑需重新发码），离线校验，服务器关停不影响已激活用户

---

## 目录结构

```
XingLianCast/
├─ sender-pc/          电脑端
│  ├─ src/             引擎源码（C++：采集/编码/协议/授权/音频）
│  ├─ ui/              界面源码（C# WinForms）+ 构建脚本
│  └─ build/           CMake 构建目录（不入库）
├─ receiver-tv/        电视端（Java，手工 aapt2/javac/d8 流水线）
├─ installer/          安装程序（C#）+ 发码工具 + payload
├─ protocol/           PJS 协议抓包与说明
├─ docs/               文档
├─ tools/              构建/测试/图标生成脚本
├─ third_party/        fdk-aac（AAC 编码，自定义许可）
└─ signing/            签发私钥与代码签名证书（**不入库**）
```

---

## 从源码构建

```bat
:: 引擎
call "D:\Dev\VS2022BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd sender-pc\build
cmake -S .. -B . -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
nmake                        :: 产物 XingLianCastSender.exe

:: 界面（含混淆 + 签名）
cd ..\ui
obf.bat                      :: 产物 XingLianCast.exe

:: 电视端
cd ..\..\receiver-tv
powershell -ExecutionPolicy Bypass -File build.ps1
```

环境依赖与发布流程见 [`docs/开发指南.md`](docs/开发指南.md)。

> **交接/待办**：`docs/明日继续-2026-11-15.md`（当前版本 PC 1.0.16 / TV 1.0.15，剩余待办与注意事项）

---

## 许可

- 本项目为**商业软件**，未授权不得分发、反编译或用于商业用途。
- 内置 Noto Sans SC 字体子集，遵循 SIL OFL 1.1（见安装目录 `OFL.txt`）。
- `third_party/fdk-aac` 遵循其自身许可（见该目录内 LICENSE）。

© 2026 XingLianCast · 版权所有，未经许可禁止复制或再分发。
