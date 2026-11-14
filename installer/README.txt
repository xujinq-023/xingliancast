Projector sender - installer
============================

安装方式（当前）：

1) 单文件安装包（推荐，随发布分发）
   - installer\setup\Setup.cs 用 C# 构建的自解压安装程序
   - 产物：installer\dist\XingLianCast-Setup-<版本>.exe
   - 默认装到 C:\Program Files\XingLianCast（需要管理员，manifest requireAdministrator）
   - 会创建开始菜单/桌面快捷方式、导入代码签名证书、写卸载注册项

2) 便携版
   - 直接解压 installer\dist\XingLianCast-Portable-<版本>.zip 运行
   - 或运行 installer\native\1-INSTALL.bat（纯 cmd，装到 %LOCALAPPDATA%\Programs\XingLianCast）

已废弃：
   - Inno Setup 脚本 projector.iss 已删除（引用旧文件名 ProjectorUi.exe / sender.exe，
     无法编译，且与实际产物不符）

Payload 刷新
   - 重新构建 XingLianCastSender.exe / XingLianCast.exe / TV APK 后，运行 make_payload.bat
   - 打包后务必执行 tools\fix-integrity-labels.ps1，否则产物带 Low 完整性标签会导致客户机装不上

Notes
   - TV 端：adb install -r receiver-tv.apk
   - PC 日志：%LOCALAPPDATA%\星联投屏\logs\XingLianCast.log（轮换，默认 5 MB，界面可设 5..100 MB）
   - TV 日志：默认开启，adb pull /sdcard/Android/data/tv.projector.receiver/files/logs/receiver.log
