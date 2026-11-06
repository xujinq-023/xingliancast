# sender-pc build (PS 5.1 safe: ASCII only). Usage: powershell -File tools\build-sender.ps1 [-Clean]
param([switch]$Clean)
$root = 'D:\DSH-Workplace\projector\sender-pc'
. D:\DSH-Workplace\projector\tools\env.ps1
$build = Join-Path $root 'build'
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
New-Item -ItemType Directory -Force -Path $build | Out-Null
Push-Location $build
cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release ..
$ok = $LASTEXITCODE -eq 0
if ($ok) { nmake; $ok = $LASTEXITCODE -eq 0 }
Pop-Location
if (-not $ok) { exit 1 }
Write-Output "OK => $build\sender.exe"
