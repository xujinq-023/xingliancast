@echo off
rem ================================================================
rem  XingLianCast - native uninstaller (no .NET)
rem  Right click -> Run as administrator
rem ================================================================
setlocal
title XingLianCast uninstall

net session >nul 2>&1
if errorlevel 1 (
  echo  [!] Need administrator rights. Right click, Run as administrator.
  pause
  exit /b 1
)

set "DST=%ProgramFiles%\XingLianCast"

echo.
echo  Uninstall XingLianCast from: %DST%
echo.
pause

echo  [1/4] stopping process...
taskkill /f /im XingLianCast.exe >nul 2>&1
taskkill /f /im XingLianCastSender.exe >nul 2>&1
ping -n 2 127.0.0.1 >nul

echo  [2/4] removing shortcuts...
del /f /q "%ProgramData%\Microsoft\Windows\Start Menu\Programs\XingLianCast.lnk" >nul 2>&1
del /f /q "%USERPROFILE%\Desktop\XingLianCast.lnk" >nul 2>&1

echo  [3/4] removing uninstall entry...
reg delete "HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\XingLianCast" /f >nul 2>&1
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\XingLianCast" /f >nul 2>&1

echo  [4/4] deleting files...
if exist "%DST%" (
  rmdir /s /q "%DST%"
  if exist "%DST%" (
    echo       [!] some files are locked, scheduling delete on reboot
    move "%DST%" "%TEMP%\XingLianCast-old" >nul 2>&1
  )
)

echo.
echo  Done. Your license is kept in %LOCALAPPDATA%.
echo.
pause
