@echo off
mkdir "%LOCALAPPDATA%\Projector\logs" 2>err1.txt
echo result=%ERRORLEVEL% > "D:\DSH-Workplace\XingLianCast\sender-pc\probe\mk_result.txt"
