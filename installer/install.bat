@echo off
rem Projector sender - portable per-user installer (ASCII only; cmd codepage safe).
rem The bilingual UI is provided by the Inno Setup package (projector.iss).
setlocal EnableDelayedExpansion
set "SRC=%~dp0payload"
set "DEST=%LOCALAPPDATA%\Programs\Projector"
if not exist "%LOCALAPPDATA%\Programs" set "DEST=%ProgramFiles%\Projector"

echo.
echo === Projector sender - install ===
echo Target: %DEST%
echo.

if not exist "%DEST%" mkdir "%DEST%" >nul 2>&1
if not exist "%DEST%" (
  echo [ERROR] cannot create %DEST%
  pause
  exit /b 1
)

copy /y "%SRC%\XingLianCastSender.exe"        "%DEST%\" >nul || goto :copyfail
copy /y "%SRC%\XingLianCast.exe"   "%DEST%\" >nul || goto :copyfail
copy /y "%SRC%\receiver-tv.apk"   "%DEST%\" >nul 2>&1
copy /y "%SRC%\autostart.ps1"     "%DEST%\" >nul
copy /y "%SRC%\start_hidden.vbs"  "%DEST%\" >nul
copy /y "%SRC%\autostart.cfg"     "%DEST%\" >nul
copy /y "%SRC%\OFL.txt"          "%DEST%\" >nul
copy /y "%~dp0uninstall.bat"      "%DEST%\" >nul
echo [1/4] files copied
goto :shortcuts
:copyfail
echo [ERROR] copy failed - check %SRC%
pause
exit /b 1

:shortcuts
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ws=New-Object -ComObject WScript.Shell; $t='%DEST%\XingLianCast.exe'; $d=@((Join-Path ([Environment]::GetFolderPath('Programs')) 'XingLianCast.lnk'),(Join-Path ([Environment]::GetFolderPath('Desktop')) 'XingLianCast.lnk')); foreach($l in $d){ $s=$ws.CreateShortcut($l); $s.TargetPath=$t; $s.WorkingDirectory='%DEST%'; $s.IconLocation=$t+',0'; $s.Save() }"
echo [2/4] shortcuts created

netsh advfirewall firewall show rule name="Projector Discovery" >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -Verb RunAs -Wait -WindowStyle Hidden -FilePath netsh -ArgumentList 'advfirewall','firewall','add','rule','name=Projector Discovery','dir=in','action=allow','protocol=UDP','localport=45691'"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -Verb RunAs -Wait -WindowStyle Hidden -FilePath netsh -ArgumentList 'advfirewall','firewall','add','rule','name=Projector Feedback','dir=in','action=allow','protocol=UDP','localport=45692'"
  if errorlevel 1 echo [WARN] firewall rule not added - TV auto-discovery may fail
) else (
  echo [3/4] firewall rule already present
)
if errorlevel 0 echo [3/4] firewall rule checked

reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\XingLianCast" /v DisplayName /t REG_SZ /d "星联投屏" /f >nul
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\XingLianCast" /v DisplayVersion /t REG_SZ /d "0.1" /f >nul
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\XingLianCast" /v Publisher /t REG_SZ /d "XingLianCast" /f >nul
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\XingLianCast" /v InstallLocation /t REG_SZ /d "%DEST%" /f >nul
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\XingLianCast" /v UninstallString /t REG_SZ /d "\"%DEST%\uninstall.bat\"" /f >nul
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\XingLianCast" /v NoModify /t REG_DWORD /d 1 /f >nul
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\XingLianCast" /v NoRepair /t REG_DWORD /d 1 /f >nul
echo [4/4] uninstall entry registered

echo.
echo Done. Start menu / Desktop: "星联投屏"
echo   - Autostart: tick the checkbox in the UI (off by default)
echo   - TV side: install receiver-tv.apk on the TV (adb install)
echo   - Logs: %LOCALAPPDATA%\Projector\logs\sender.log
echo.
choice /c YN /n /m "Launch 星联投屏 now? [Y/N] "
if errorlevel 2 goto :done
start "" "%DEST%\XingLianCast.exe"
:done
endlocal
exit /b 0