@echo off
rem Enable "start at logon" (hidden window launcher). NOT enabled by default.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0autostart.ps1" enable
pause
