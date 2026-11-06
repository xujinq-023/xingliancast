# M0 环境验收清单

## 本机（自动完成项）
| 项 | 位置 | 状态 |
|---|---|---|
| JDK 17 (Microsoft OpenJDK 17.0.21) | C:\Program Files\Microsoft\jdk-17.0.21.21-hotspot，`JAVA_HOME` 已设 | ✅ `java -version` 通过 |
| Android SDK（platform-tools / android-34 / build-tools 34） | D:\Dev\Android\Sdk，`ANDROID_HOME` 已设，licenses 已接受 | ✅ `adb version 1.0.41` 通过 |
| cmdline-tools（sdkmanager/avdmanager） | D:\Dev\Android\cmdline-tools\latest\bin | ✅ 已入 PATH |
| VS 2022 BuildTools（MSVC 14.44 + Windows SDK 10.0.26100 + CMake 3.31，VCTools 工作负载） | D:\Dev\VS2022BuildTools | ✅ C++20 编译-链接-运行冒烟通过（MSVC-OK 42） |
| Wireshark 4.6.9 + Npcap 1.82（tshark） | C:\Program Files\Wireshark | ✅ `tshark -v` 通过 + 实测抓包 8 帧（winget CDN 卡死 → 官网直连；Npcap 单独装） |
| jadx 1.5.0（便携） | D:\Dev\tools\jadx\bin\jadx.bat | ✅ `--version` 通过（需 JAVA_HOME，脚本内已处理） |

> 注：Gradle 采用 wrapper 方式（项目内自动下载），暂不安装 Android Studio。
> 注：本 harness 进程持有旧 PATH（环境变量改动只对新进程生效），**所有构建/工具脚本开头须 source `tools/env.ps1`**（编码阶段第一项交付）。

## 电视侧（需要你手动操作 — M0 的最终验收）
1. **开启开发者模式**：智慧屏 设置 → 关于 → 连续点击"版本号"×7（或"设备信息→版本号"），直到提示已进入开发者模式。
2. **开启 ADB 调试（局域网）**：设置 → 开发者选项 → ADB 调试 打开。
3. **电视 IP = 10.5.9.22（已提供）**；开发者模式暂未开启 → 待开启后执行
   `adb connect 10.5.9.22:5555` → `adb devices` 验证 → 安装 Hello/接收端 APK 验证侧载。
4. 备选：若网络 adb 不可用，用 U 盘安装（官方文档支持），或智慧屏手机 App 推送安装。

## 验收通过标准
- [x] `java -version` / `adb version` / `sdkmanager --list` 正常
- [x] `cl.exe` + `cmake --version`（经 VsDevCmd）正常 —— C++20 编译+链接+运行 全通过
- [x] `tshark -v`（4.6.9）、`jadx --version`（1.5.0）正常；Npcap 1.82 已装，以太网口实测抓到 8 包
- [ ] `adb connect <TV_IP>:5555` 成功且 `adb shell getprop ro.build.version.incremental` 可读
- [ ] Hello APK `adb install` 成功并在电视上能打开
