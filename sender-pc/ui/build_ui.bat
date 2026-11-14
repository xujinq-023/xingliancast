@echo off
rem Build the projector sender UI (WinForms, .NET Framework 4.x).
rem Output: sender-pc\ui\XingLianCast.exe  (copy next to XingLianCastSender.exe to run)
setlocal
set CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe
if not exist "%CSC%" set CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe
if not exist "%CSC%" (
  echo csc.exe not found - .NET Framework 4.x is required
  exit /b 1
)
cd /d "%~dp0"
"%CSC%" /nologo /target:winexe /optimize+ /out:XingLianCast.exe ^
  /r:System.dll /r:System.Core.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll ^
  /resource:logo.png,logo.png ^
  /resource:fonts\XLUI.ttf,XLUI.ttf ^
  /win32icon:logo.ico ^
  ProjectorUi.cs
if errorlevel 1 exit /b 1
copy /y fonts\OFL.txt OFL.txt >nul
echo BUILD OK: %~dp0XingLianCast.exe