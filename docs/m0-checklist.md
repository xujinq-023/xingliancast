# M0 环境验收清单

## 本机（自动完成项）
| 项 | 位置 | 状态 |
|---|---|---|
| JDK 17 (Microsoft OpenJDK 17.0.21) | C:\Program Files\Microsoft\jdk-17.0.21.21-hotspot，`JAVA_HOME` 已设 | ✅ `java -version` 通过 |
| Android SDK（platform-tools / android-34 / build-tools 34） | D:\Dev\Android\Sdk，`ANDROID_HOME` 已设，licenses 已接受 | ✅ `adb version 1.0.41` 通过 |
| cmdline-tools（sdkmanager/avdmanager） | D:\Dev\Android\cmdline-tools\latest\bin | ✅ 已入 PATH |
| VS 2022 BuildTools（MSVC + Windows SDK + CMake，VCTools 工作负载） | D:\Dev\VS2022BuildTools | ⏳ 安装中（约 6–7GB） |
| Wireshark + Npcap | C:\Program Files\Wireshark | ⏳ 排队（BuildTools 之后） |
| jadx 1.5.0（便携） | D:\Dev\tools\jadx\bin\jadx.bat | ⏳ 排队（最后） |

> 注：Gradle 采用 wrapper 方式（项目内自动下载），暂不安装 Android Studio；CMake/编译器经 VS 的 `LaunchToolsShell`（VsDevCmd）使用，build 脚本会显式调用。

## 电视侧（需要你手动操作 — M0 的最终验收）
1. **开启开发者模式**：智慧屏 设置 → 关于 → 连续点击"版本号"×7（或"设备信息→版本号"），直到提示已进入开发者模式。
2. **开启 ADB 调试（局域网）**：设置 → 开发者选项 → ADB 调试 打开。
3. **确认电视 IP**（设置 → 网络，与本机同网段 10.5.9.x）并告知我，我执行：
   `adb connect <TV_IP>:5555` → `adb devices` 验证 → 安装 Hello APK 验证侧载。
4. 备选：若网络 adb 不可用，用 U 盘安装（官方文档支持），或智慧屏手机 App 推送安装。

## 验收通过标准
- [x] `java -version` / `adb version` / `sdkmanager --list` 正常
- [ ] `cl.exe` + `cmake --version`（经 VsDevCmd）正常
- [ ] `tshark -v`、`jadx --version` 正常
- [ ] `adb connect <TV_IP>:5555` 成功且 `adb shell getprop ro.build.version.incremental` 可读
- [ ] Hello APK `adb install` 成功并在电视上能打开
