@echo off
rem ================================================================
rem  XingLianCast - native installer (no .NET, uses only cmd tools)
rem  Right click -> Run as administrator
rem ================================================================
setlocal EnableExtensions
title XingLianCast setup

net session >nul 2>&1
if errorlevel 1 (
  echo.
  echo  [!] Need administrator rights.
  echo      Close this window, right click 1-INSTALL.bat,
  echo      choose "Run as administrator", then try again.
  echo.
  pause
  exit /b 1
)

set "SRC=%~dp0payload"
set "DST=%ProgramFiles%\XingLianCast"
if not exist "%SRC%\XingLianCast.exe" (
  echo  [!] payload folder not found next to this script.
  pause
  exit /b 1
)

echo.
echo  Install to: %DST%
echo.

echo  [1/5] stopping running instance...
taskkill /f /im XingLianCast.exe >nul 2>&1
taskkill /f /im XingLianCastSender.exe >nul 2>&1
ping -n 2 127.0.0.1 >nul

echo  [2/5] copying files...
if not exist "%DST%" mkdir "%DST%" 2>nul
xcopy /y /e /i /q "%SRC%\*" "%DST%\" >nul 2>&1
if not exist "%DST%\XingLianCast.exe" (
  echo  [!] cannot write to "%DST%", falling back to user folder...
  set "DST=%LOCALAPPDATA%\Programs\XingLianCast"
  if not exist "%DST%" mkdir "%DST%" 2>nul
  xcopy /y /e /i /q "%SRC%\*" "%DST%\" >nul 2>&1
)
if not exist "%DST%\XingLianCast.exe" (
  echo  [!] copy failed even to user folder.
  echo      Please run this file as administrator, or edit DST in this file.
  pause
  exit /b 1
)
echo      installed to: %DST%

echo  [3/5] installing code signing certificate...
certutil -addstore -f Root "%DST%\xinglian-codesign.cer" >nul 2>&1
certutil -addstore -f TrustedPublisher "%DST%\xinglian-codesign.cer" >nul 2>&1

echo  [4/5] creating shortcuts...
set "VBS=%TEMP%\xl_mklnk.vbs"
> "%VBS%" echo Set ws = CreateObject("WScript.Shell")
>> "%VBS%" echo Set s = ws.CreateShortcut(WScript.Arguments(0))
>> "%VBS%" echo s.TargetPath = WScript.Arguments(1)
>> "%VBS%" echo s.WorkingDirectory = WScript.Arguments(2)
>> "%VBS%" echo s.IconLocation = WScript.Arguments(1) ^& ",0"
>> "%VBS%" echo s.Description = "XingLianCast"
>> "%VBS%" echo s.Save
cscript //nologo "%VBS%" "%ProgramData%\Microsoft\Windows\Start Menu\Programs\XingLianCast.lnk" "%DST%\XingLianCast.exe" "%DST%" >nul 2>&1
cscript //nologo "%VBS%" "%USERPROFILE%\Desktop\XingLianCast.lnk" "%DST%\XingLianCast.exe" "%DST%" >nul 2>&1
del /f /q "%VBS%" >nul 2>&1

echo  [5/5] registering uninstall entry...
reg add "HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\XingLianCast" /v DisplayName /t REG_SZ /d "XingLianCast" /f >nul 2>&1
reg add "HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\XingLianCast" /v DisplayVersion /t REG_SZ /d "1.0.3" /f >nul 2>&1
reg add "HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\XingLianCast" /v Publisher /t REG_SZ /d "XingLianCast" /f >nul 2>&1
reg add "HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\XingLianCast" /v InstallLocation /t REG_SZ /d "%DST%" /f >nul 2>&1
reg add "HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\XingLianCast" /v DisplayIcon /t REG_SZ /d "%DST%\XingLianCast.exe" /f >nul 2>&1
reg add "HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\XingLianCast" /v UninstallString /t REG_SZ /d "\"%DST%\uninstall.exe\"" /f >nul 2>&1
reg add "HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\XingLianCast" /v NoModify /t REG_DWORD /d 1 /f >nul 2>&1
reg add "HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\XingLianCast" /v NoRepair /t REG_DWORD /d 1 /f >nul 2>&1

echo.
echo  Done. Starting XingLianCast...
start "" "%DST%\XingLianCast.exe"
echo.
pause
