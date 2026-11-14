@echo off
rem Build UI, obfuscate with ConfuserEx, then Authenticode-sign the result.
setlocal
cd /d "%~dp0"

call build_ui.bat
if errorlevel 1 goto :err

set CONFUSER=D:\DSH-Workplace\XingLianCast\build-tools\confuserex\tools\Confuser.CLI.exe
if not exist "%CONFUSER%" (
  echo [WARN] ConfuserEx not found - skipping obfuscation, using plain build
  goto :sign
)

echo [1/3] Obfuscating...
if exist "obf" rmdir /s /q "obf"
mkdir "obf"
copy /y "ProjectorUi.exe" "obf\ProjectorUi.exe" >nul
"%CONFUSER%" "%~dp0project.crproj" >nul 2>&1
if not exist "obf\ProjectorUi.exe" goto :obffail

echo [2/3] Replacing build output with obfuscated binary...
copy /y "obf\ProjectorUi.exe" "ProjectorUi.exe" >nul
echo [3/3] Signing...
goto :sign

:sign
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$c=Get-Item 'Cert:\CurrentUser\My\C298644FCF4FDBE22B49BA99472A4E50AC217E27' -ErrorAction SilentlyContinue; if($c){ $s=Set-AuthenticodeSignature -FilePath 'ProjectorUi.exe' -Certificate $c; Write-Host ('sign: '+$s.Status) } else { Write-Host 'sign: certificate not found' }"
echo [OK] ProjectorUi.exe ready
goto :eof

:obffail
echo [ERROR] ConfuserEx failed, keeping plain build
goto :sign

:err
echo [ERROR] build_ui.bat failed
exit /b 1
