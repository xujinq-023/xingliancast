@echo off
call "D:\Dev\VS2022BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d D:\DSH-Workplace\build-asan
set ASAN_OPTIONS=halt_on_error=1:exitcode=99
"D:\DSH-Workplace\build-asan\XingLianCastSender.exe" --ip 10.5.9.22
