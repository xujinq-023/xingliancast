@echo off
rem Re-stage the installer payload from the current builds.
rem Usage: make_payload.bat  (run after rebuilding XingLianCastSender.exe / XingLianCast.exe / APK)
setlocal
set "ROOT=%~dp0"
set "SRC=%~dp0.."
if not exist "%ROOT%payload" mkdir "%ROOT%payload"
copy /y "%SRC%\sender-pc\build\XingLianCastSender.exe"       "%ROOT%payload\" >nul || goto :err
copy /y "%SRC%\sender-pc\build\XingLianCast.exe"  "%ROOT%payload\" >nul || goto :err
copy /y "%SRC%\sender-pc\autostart.ps1"          "%ROOT%payload\" >nul || goto :err
copy /y "%SRC%\sender-pc\start_hidden.vbs"       "%ROOT%payload\" >nul || goto :err
copy /y "%SRC%\sender-pc\autostart.cfg"          "%ROOT%payload\" >nul || goto :err
copy /y "%SRC%\receiver-tv\out\receiver-tv-debug.apk" "%ROOT%payload\receiver-tv.apk" >nul
echo Payload staged:
dir /b "%ROOT%payload"
exit /b 0
:err
echo [ERROR] missing build output - build XingLianCastSender.exe / XingLianCast.exe first
exit /b 1