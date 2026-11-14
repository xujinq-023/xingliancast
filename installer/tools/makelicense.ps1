# 生成「星联投屏」注册码
# 用法: powershell -ExecutionPolicy Bypass -File makelicense.ps1 XXXX-XXXX-XXXX
# 机器码在客户端「☰ → 正版注册」里显示
param([Parameter(Mandatory=$true)][string]$MachineCode)

$secret = 'XL-2026-PROJECTOR-SIGNKEY-7f3a91c4'
$raw = $secret + '|' + $MachineCode.Trim().ToUpperInvariant()
$sha = [System.Security.Cryptography.SHA256]::Create()
$hash = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($raw))
$hex = -join ($hash[0..5] | ForEach-Object { $_.ToString('X2') })
$key = 'XL-' + $hex.Substring(0,4) + '-' + $hex.Substring(4,4) + '-' + $hex.Substring(8,4)
Write-Host ""
Write-Host "机器码: $MachineCode"
Write-Host "注册码: $key" -ForegroundColor Green
Write-Host ""
