@echo off
rem Projector sender - uninstaller (ASCII only).
setlocal EnableDelayedExpansion
set "DEST=%~dp0"
if "%DEST:~-1%"=="\" set "DEST=%DEST:~0,-1%"

echo.
echo === Projector sender - uninstall ===
echo Folder: %DEST%
echo.
choice /c YN /n /m "Uninstall? [Y/N] "
if errorlevel 2 exit /b 0

echo [1/5] stopping processes
taskkill /f /im ProjectorUi.exe >nul 2>&1
taskkill /f /im sender.exe >nul 2>&1

echo [2/5] removing shortcuts
powershell -NoProfile -ExecutionPolicy Bypass -Command "$l=@((Join-Path ([Environment]::GetFolderPath('Programs')) 'Projector Sender.lnk'),(Join-Path ([Environment]::GetFolderPath('Startup')) 'Projector Sender.lnk'),(Join-Path ([Environment]::GetFolderPath('Desktop')) 'Projector Sender.lnk')); foreach($f in $l){ if(Test-Path $f){ Remove-Item $f -Force } }"

echo [3/5] removing firewall rule
netsh advfirewall firewall show rule name="Projector Discovery" >nul 2>&1
if not errorlevel 1 powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -Verb RunAs -Wait -WindowStyle Hidden -FilePath netsh -ArgumentList 'advfirewall','firewall','delete','rule','name=Projector Discovery'"
if not errorlevel 1 powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -Verb RunAs -Wait -WindowStyle Hidden -FilePath netsh -ArgumentList 'advfirewall','firewall','delete','rule','name=Projector Feedback'"

echo [4/5] removing uninstall entry
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\ProjectorSender" /f >nul 2>&1

echo [5/5] removing program files
start "" /min cmd /c "timeout /t 2 >nul & rmdir /s /q ""%DEST%"""
echo.
echo Done. Logs in %LOCALAPPDATA%\Projector\logs are kept (delete manually if needed).
endlocal
exit /b 0