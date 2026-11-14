# Run ASan XingLianCastSender.exe for 240s; report ASan errors or survival.
$ErrorActionPreference = 'Continue'
$exe = 'D:\DSH-Workplace\XingLianCast\sender-pc\build-asan\XingLianCastSender.exe'
$log = "$env:TEMP\pjs_asan_out.txt"
Remove-Item $log -ErrorAction SilentlyContinue
$vcvars = 'D:\Dev\VS2022BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$bat = 'D:\DSH-Workplace\XingLianCast\tools\asan_env.bat'
$errlog = "$env:TEMP\pjs_asan_err.txt"
Remove-Item $errlog -ErrorAction SilentlyContinue
$p = Start-Process cmd -ArgumentList '/c', $bat -PassThru -WindowStyle Hidden `
     -RedirectStandardOutput $log -RedirectStandardError $errlog
$pid2 = $p.Id
$deadline = (Get-Date).AddSeconds(240)
$alive = $true
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Seconds 10
  $procs = Get-Process sender -ErrorAction SilentlyContinue
  if (-not $procs) { $alive = $false; break }
}
if ($alive) {
  Get-Process sender -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Seconds 2
  Write-Output 'RESULT: SURVIVED 240s (killed)'
} else {
  Write-Output 'RESULT: DIED'
}
Write-Output '--- asan stdout tail ---'
if (Test-Path $log) { Get-Content $log -Tail 40 } else { Write-Output '(no log)' }
Write-Output '--- asan stderr tail ---'
if (Test-Path $errlog) { Get-Content $errlog -Tail 60 } else { Write-Output '(no err)' }
Write-Output '--- newest dumps ---'
cmd /c "dir /T:W D:\DSH-Workplace\wdumps\*.dmp 2>nul | findstr dmp"
