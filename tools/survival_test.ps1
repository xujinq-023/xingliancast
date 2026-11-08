# 页堆生存测试：启动 sender 5.5 分钟，检查存活 + 事件日志 + 新 dump
param([string]$ExtraArgs = '')
$ErrorActionPreference = 'Continue'
$exe = 'D:\DSH-Workplace\projector\sender-pc\build\sender.exe'
$log = "$env:TEMP\pjs_tv_pageheap.txt"
Remove-Item $log,$env:TEMP\pjs_tv_pageheap.err.txt -EA SilentlyContinue
$d0 = (Get-ChildItem 'D:\DSH-Workplace\wdumps\*.dmp' -EA SilentlyContinue).Count
$t0 = Get-Date
$argl = @('--ip','10.5.9.22')
if ($ExtraArgs) { $argl += ($ExtraArgs -split '\s+') }
$p = Start-Process $exe -ArgumentList $argl -RedirectStandardOutput $log -RedirectStandardError "$env:TEMP\pjs_tv_pageheap.err.txt" -PassThru -WindowStyle Hidden
"started PID=$($p.Id) at $t0"
$alive = $true
for ($i = 0; $i -lt 33; $i++) {
  Start-Sleep -Seconds 10
  $p.Refresh()
  if ($p.HasExited) { $alive = $false; "DIED after $(($i+1)*10)s exit=$($p.ExitCode)"; break }
  if ($i % 6 -eq 5) { "t=$($((($i+1)*10)))s alive threads=$($p.Threads.Count)" }
}
if ($alive) { "RESULT: ALIVE 330s — killing"; Stop-Process -Id $p.Id -Force }
$d1 = (Get-ChildItem 'D:\DSH-Workplace\wdumps\*.dmp' -EA SilentlyContinue).Count
"dumps: $d0 -> $d1"
"--- 最近 Application 错误事件(10min 内):"
Get-WinEvent -FilterHashtable @{LogName='Application'; Level=2; StartTime=(Get-Date).AddMinutes(-10)} -MaxEvents 5 -EA SilentlyContinue |
  ForEach-Object { "[$($_.TimeGenerated)] $($_.ProviderName): $(($_.Message -split "`n")[0..2] -join ' | ')" }
"--- 日志尾部:"
Get-Content $log -Tail 6 -EA SilentlyContinue
"--- stderr 尾部:"
Get-Content "$env:TEMP\pjs_tv_pageheap.err.txt" -Tail 4 -EA SilentlyContinue
