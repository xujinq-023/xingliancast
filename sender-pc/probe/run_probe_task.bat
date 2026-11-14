@echo off
call "D:\Dev\VS2022BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d D:\DSH-Workplace\XingLianCast\sender-pc\probe
cl /nologo /EHsc /std:c++20 /utf-8 /O2 probe.cpp /Fe:probe.exe /link windowsapp.lib runtimeobject.lib d3d11.lib dxgi.lib ole32.lib user32.lib uuid.lib > build.log 2>&1
probe.exe > D:\DSH-Workplace\pjs_probe.log 2>&1
