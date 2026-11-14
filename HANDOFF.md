# 星联投屏 · 会话交接快照

> 快照时间：2026-11-10 23:00
> 详细历史见 `STATUS.md`（44 节工程日志）；本文件只记录**当前状态、已知问题、下一步**。

---

## 一、产品形态

| 组件 | 说明 |
|---|---|
| **PC 发送端** | `sender.exe`（引擎）+ `ProjectorUi.exe`（界面），Windows，私有 PJS 协议 |
| **TV 接收端** | `receiver-tv.apk`，Android 12（华为 QINL-370A 智慧屏），UDP 45690 收流 |
| 发现 | TV 每秒向 UDP 45691 广播；PC 回传状态走 UDP 45692 |
| 品牌 | 星联投屏（窗口标题 / 托盘 / TV 应用名 / 图标） |

---

## 二、当前能力

### 画面
- DXGI Desktop Duplication 抓屏（WGC 在部分环境返回 E_ACCESSDENIED，已自动回退）
- 硬件编码优先（NVIDIA NVENC / Intel QSV 枚举），失败自动回退
- 输入路径三级回退：**内存 NV12 → GPU 纹理 → CPU 转换**，另有**编码器看门狗**（1.5 秒无输出即切下一级，最终换软件编码）
- 关键帧策略：首帧 / TV 报丢包 / 静止桌面每 2 秒补 IDR（解决"首帧丢失后永久黑屏"）
- 鼠标光标合成（黑/白双渲染反推 alpha，解决黑块）
- 多显示器选择、限帧（令牌桶）、静止画面心跳补帧

### 声音
- WASAPI loopback → AAC-LC 128k → ADTS，界面可实时开关

### 界面
- 浅色主题（`PJS_UI_THEME=dark` 可切深色），内嵌 Noto Sans SC 子集（OFL）
- 10 项实时指标卡片（数值与标题同字体同色、不加粗）
- 底栏三段：计算机名+IP（左）/ 消息（中）/ 版本+build（右）
- ☰ 菜单：开机自启（勾选式）/ 打开日志文件夹 / 正版注册 / 关于 / 退出
- 正版注册：机器码 `XXXX-XXXX-XXXX` + 注册码 `XL-XXXX-XXXX-XXXX`（SHA256，见 `tools/makelicense.ps1`）

### TV 端
- 四态：等待投屏 / 正在连接 / 连接已断开（冻结最后一帧 + 顶部提示条 3 秒）/ 接入失败
- 按键提示、MENU 三态信息条（隐藏 / 分辨率帧率 / 诊断浮层）
- 图标：满幅紫色渐变 + 霓虹环（16/24px 用简化版）

---

## 三、实测性能

| 机器 | 编码 | 帧率 | 备注 |
|---|---|---|---|
| 本机（GTX 1050 Ti） | NVIDIA MFT | **60 fps**，编码 2ms，转换 1.7ms | 正常路径 |
| 另一台（Intel 核显） | 软件编码 + GPU 转换 | **~45 fps**，编码 12ms，转换 2ms | QSV 不可用，见下 |

---

## 四、已知问题（按优先级）

### 1. Intel QSV 硬件编码不可用（中）
- 现象：选中 `Intel® Quick Sync Video H.264 Encoder MFT`（async, d3d=1）后 **既不报错也不出帧**
- 已尝试：内存 NV12 输入、GPU 纹理直入、CPU 转换 —— 三种都 0 帧
- 现状：看门狗自动切到**软件编码**（~45fps）
- 待办：调 QSV 的协商顺序（`MFT_MESSAGE_SET_D3D_MANAGER` 时机、`MF_MT_*` 属性、`CODECAPI` 设置），
  目标是 60fps

### 2. 部分机器无法创建托盘图标（低）
- 根因：`EnableLUA=0` 时 explorer 运行在 High，普通进程是 Medium，UIPI 禁止注册通知图标
- 现状：点 × 改为**最小化到任务栏**（不会丢窗口）；UAC 正常的机器托盘图标可用

### 3. 安装程序在个别机器被拦截（低）
- 现象：`Program Files` / `AppData\Local` / 用户目录全部拒绝写入
- 现状：三级路径回退 + 逐路径报错原因；另有**便携版 zip**（解压即用）绕过

### 4. 版权控制未实现（待决策，见下）

---

## 五、下一步（用户已确认方向，但方案未定稿）

### A. 版权控制
已给出三方案（人工发码 / **自助网页发码** / 在线激活），用户倾向先讨论清楚再动手。
关键决策点：
1. 方案选型（推荐自助网页发码 + 离线验签）
2. 计费模型（买断 / 年费）
3. 支付通道（需用户提供商户号或用第三方聚合支付）
4. TV 端是否绑定 PC（推荐"PC 主授权 + TV 白名单，最多 3 台"）
5. 是否加 7 天试用

**技术前提**：把现在的 HMAC 换掉（密钥硬编码在 exe 里可被逆），改用**非对称签名**
（服务器私钥签、客户端只存公钥）。这一步无论选哪个方案都要做。

### B. 文档
- 中英文使用说明 + 界面/电视端截图
- 当前 `installer\dist\安装说明.txt` 只覆盖安装与排障

### C. 收尾
- 安装包版本号统一（readme 里还是 0.1.0，实际已到 0.1.7）
- `build\` 下积累了大量测试日志（~200 个文件），可清理

---

## 六、产物位置

| 路径 | 内容 |
|---|---|
| `sender-pc\src\` | 引擎源码（main.cpp 841 行、encoder.h 380 行） |
| `sender-pc\build\` | `sender.exe`、`ProjectorUi.exe`、CMake 构建目录 |
| `sender-pc\ui\` | 界面源码（1939 行）、图标、字体、`build_ui.bat` |
| `receiver-tv\src\` | TV 端 Java 源码（RxEngine 879 行、MainActivity 418 行） |
| `receiver-tv\out\` | `receiver-tv-debug.apk` |
| `installer\dist\` | **交付物**：安装包、便携版 zip、APK、证书、说明 |
| `installer\setup\` | 安装程序源码 `Setup.cs` + `payload.zip` |
| `signing\` | 自签名证书 (.pfx/.cer) + `sign.ps1` |
| `installer\tools\` | `makelicense.ps1`（注册码生成器） |

---

## 七、如何重新构建

```powershell
# 引擎（需要 D:\Dev\VS2022BuildTools）
call "D:\Dev\VS2022BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd D:\DSH-Workplace\XingLianCast\sender-pc\build
nmake

# 界面
cd D:\DSH-Workplace\XingLianCast\sender-pc\ui
build_ui.bat

# TV 端
cd D:\DSH-Workplace\XingLianCast\receiver-tv
powershell -ExecutionPolicy Bypass -File build.ps1

# 签名（每次重编后必须重签，否则签名失效）
$cert = Get-Item "Cert:\CurrentUser\My\BB336FBB91AAE5FDDED1577CDCCFD3E83CBD77BE"
Set-AuthenticodeSignature -FilePath <exe> -Certificate $cert

# 打包（payload → zip → setup.exe）
7z a -tzip installer\setup\payload.zip installer\payload\*
csc /target:winexe /out:XingLianSetup.exe /resource:payload.zip,payload.zip ...
```

---

## 八、重要常量

| 项 | 值 |
|---|---|
| 视频/音频端口 | UDP 45690 |
| 发现广播端口 | UDP 45691（TV 广播） |
| 状态回传端口 | UDP 45692（PC 监听） |
| 协议头 | 16 字节大端（magic/type/seq/ts/flags/len） |
| 注册码密钥 | `XL-2026-PROJECTOR-SIGNKEY-7f3a91c4`（HMAC 输入，**待改非对称**） |
| 签名证书指纹 | `BB336FBB91AAE5FDDED1577CDCCFD3E83CBD77BE` |
| 证书 PFX 密码 | `XingLian!2026` |
