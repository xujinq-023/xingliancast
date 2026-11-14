# sender_watch.ps1 - keep sender alive; log exits with timestamps
param([string]$Ip = '10.5.9.22')
$exe = 'D:\DSH-Workplace\XingLianCast\sender-pc\build\XingLianCastSender.exe'
$log = Join-Path $env:TEMP 'pjs_tv.txt'
$round = 0
while ($true) {
  $round++
  Add-Content -Path $log -Value "=== round $round start $(Get-Date -Format HH:mm:ss) ===" -Encoding UTF8
  $p = Start-Process -FilePath $exe -ArgumentList '--ip',$Ip,'--fps','30','--bitrate','4000','--height','720' -RedirectStandardOutput $env:TEMP\pjs_tv_cur.txt -WindowStyle Hidden -PassThru
  Wait-Process -Id $p.Id -ErrorAction SilentlyContinue
  Add-Content -Path $log -Value "=== round $round exit code=$($p.ExitCode) $(Get-Date -Format HH:mm:ss) ===" -Encoding UTF8
  $tail = Get-Content -Path $env:TEMP\pjs_tv_cur.txt -Tail 2 -Encoding UTF8 -ErrorAction SilentlyContinue
  if ($tail) { Add-Content -Path $log -Value ($tail -join "`n") -Encoding UTF8 }
  Start-Sleep 1
}
