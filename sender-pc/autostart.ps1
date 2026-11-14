# Projector autostart switch (NOT enabled by default).
# Uses the Startup folder + hidden-window launcher: no password, no admin needed,
# starts in the user's interactive session after logon.
# ASCII only: PowerShell 5.1 reads BOM-less files as ANSI.
param([Parameter(Mandatory = $true)][ValidateSet('enable', 'disable', 'status')][string]$Action)

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$vbs  = Join-Path $here 'start_hidden.vbs'
$lnk  = Join-Path ([Environment]::GetFolderPath('Startup')) 'Projector Sender.lnk'
$ws   = New-Object -ComObject WScript.Shell

switch ($Action) {
    'enable' {
        if (-not (Test-Path $vbs)) { Write-Host "missing start_hidden.vbs: $vbs"; exit 1 }
        $s = $ws.CreateShortcut($lnk)
        $s.TargetPath = Join-Path $env:SystemRoot 'System32\wscript.exe'
        $s.Arguments = '"' + $vbs + '"'
        $s.WorkingDirectory = $here
        $s.Description = 'Projector sender (start at logon)'
        $s.Save()
        if (Test-Path $lnk) { Write-Host "autostart ENABLED: $lnk"; exit 0 }
        Write-Host 'failed to create shortcut'; exit 1
    }
    'disable' {
        if (Test-Path $lnk) { Remove-Item $lnk -Force; Write-Host "autostart DISABLED (removed $lnk)" }
        else { Write-Host 'autostart was already disabled' }
        exit 0
    }
    'status' {
        if (Test-Path $lnk) { Write-Host "autostart: ENABLED  ($lnk)" }
        else { Write-Host 'autostart: disabled' }
        exit 0
    }
}
