Projector sender - installer
============================

Two ways to install:

1) Portable installer (works now, no extra tooling)
   - Double-click install.bat
   - Installs to %LOCALAPPDATA%\Programs\Projector (per-user, no admin)
   - Creates Start Menu + Desktop shortcuts "Projector Sender"
   - Adds the inbound firewall rule for UDP 45691 (TV auto-discovery) - may prompt for admin
   - Registers an uninstall entry; uninstall.bat removes everything

2) Inno Setup package (single .exe, bilingual UI)
   - Install Inno Setup 6, then:
       iscc.exe /DMyPayloadDir="<this folder>\payload" projector.iss
   - Output: out\ProjectorSetup-0.1.exe
   - Optional tasks in the wizard: desktop icon, start with Windows (off by default),
     firewall rule for discovery

Payload refresh
   - Run make_payload.bat after rebuilding sender.exe / ProjectorUi.exe / the TV APK

Notes
   - TV side: install receiver-tv.apk on the TV (adb install -r receiver-tv.apk)
   - Logs: %LOCALAPPDATA%\Projector\logs\sender.log (rotated, 5 MB default, UI setting 5..100 MB)
   - TV logs are OFF by default; toggle with the remote MENU key (or long-press the overlay)