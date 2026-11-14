; Projector 投屏 — Inno Setup 6 script (compile with ISCC.exe to get a single setup .exe)
; 中文/English bilingual installer, per-user install (no admin needed except the
; optional firewall rule task).
;
; Build:
;   iscc.exe /DMyPayloadDir="D:\DSH-Workplace\projector\installer\payload" projector.iss

#define MyAppName "Projector 投屏"
#define MyAppVersion "0.1"
#define MyAppPublisher "Projector"
#define MyAppExeName "ProjectorUi.exe"

#ifndef MyPayloadDir
  #define MyPayloadDir "payload"
#endif

[Setup]
AppId={{7C1E5B52-2F2A-4C63-9B7B-2D0B3F6A9C11}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\Programs\Projector
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=out
OutputBaseFilename=ProjectorSetup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; TV 端 APK 单独分发，不参与主安装
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "zh"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart";   Description: "开机自启 / Start with Windows"; GroupDescription: "启动选项 / Startup"; Flags: unchecked
Name: "firewall";    Description: "放行 UDP 45691（设备自动发现）/ Allow UDP 45691 for discovery"; GroupDescription: "网络 / Network"; Flags: checkedonce

[Files]
Source: "{#MyPayloadDir}\sender.exe";       DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyPayloadDir}\ProjectorUi.exe";  DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyPayloadDir}\autostart.ps1";    DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyPayloadDir}\start_hidden.vbs"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyPayloadDir}\autostart.cfg";    DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyPayloadDir}\receiver-tv.apk";  DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#MyPayloadDir}\OFL.txt";  DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; 防火墙规则：用 netsh，提权执行（PrivilegesRequired=lowest 下 runascurrentuser 不足以改防火墙，
; 所以这里用 runasoriginaluser 的提权子进程）
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""Projector Discovery"" dir=in action=allow protocol=UDP localport=45691"; Flags: runhidden runascurrentuser; Tasks: firewall; Check: not RuleExists
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""Projector Feedback"" dir=in action=allow protocol=UDP localport=45692"; Flags: runhidden runascurrentuser; Tasks: firewall
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Projector Discovery"""; Flags: runhidden runascurrentuser

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
function RuleExists: Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec('netsh', 'advfirewall firewall show rule name="Projector Discovery"', '', SW_HIDE,
                 ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then begin
    if WizardIsTaskSelected('autostart') then
      Exec('powershell', '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{app}\autostart.ps1') + '" enable',
           '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;
