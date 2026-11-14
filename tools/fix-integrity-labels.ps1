# 发布产物完整性标签修正脚本
#
# 背景：如果安装包/APK/便携版是在「低完整性」目录里生成的（例如自动化沙箱、
# Codex/DSH 之类的工作区），产出的文件会带上 "Mandatory Label\Low Mandatory Level"。
# 这种文件在客户机双击时会以【低完整性】运行，导致：
#   · 任何目录都写不进去（报 "无法写入任何候选目录" / 访问被拒绝）
#   · 即使右键「以管理员身份运行」也没用
# 症状很好认：icacls <exe> 最后一行出现 "Low Mandatory Level"
#
# 本脚本把指定目录下的文件重置 ACL 继承，并显式设为 Medium 完整性。
#
# 用法（管理员 PowerShell）：
#   powershell -ExecutionPolicy Bypass -File tools\fix-integrity-labels.ps1 "D:\out\dist"

param([Parameter(Mandatory = $true)][string]$Dir)

if (-not (Test-Path $Dir)) { Write-Host "not found: $Dir"; exit 1 }

Write-Host "resetting ACL inheritance under: $Dir"
cmd /c "icacls `"$Dir`" /reset /t /c /q" | Out-Null

Write-Host "setting mandatory label to Medium"
cmd /c "icacls `"$Dir`" /setintegritylevel (OI)(CI)Medium /t /c /q" | Out-Null

Write-Host ""
Write-Host "check:"
Get-ChildItem $Dir -File | ForEach-Object {
    $line = (cmd /c "icacls `"$($_.FullName)`"" 2>$null | Select-String 'Mandatory Label')
    $lbl = if ($line) { ($line.Line -split ':')[0].Trim() } else { '(none)' }
    "{0,-40} {1}" -f $_.Name, $lbl
}
Write-Host ""
Write-Host "Expect: 'Medium Mandatory Level'. If you still see 'Low', re-run as administrator."
