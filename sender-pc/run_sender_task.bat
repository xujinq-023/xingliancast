@echo off
rem 计划任务启动 sender（脱离 DSH 作业对象，harness 重启不影响）
cd /d D:\DSH-Workplace\XingLianCast\sender-pc\build
start "" /min cmd /c "XingLianCastSender.exe --height 1440 > run_task.log 2>&1"
