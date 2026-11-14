# Regenerate the embedded UI font subset (fonts\XLUI.ttf).
# Source: Noto Sans SC (variable, SIL OFL 1.1) -> weight 600 instance -> subset to the
# characters actually used in ProjectorUi.cs -> family renamed to "XL UI".
# Run this whenever new UI strings introduce characters that are not in the subset,
# otherwise those characters render as blank boxes in the title / metric values.
param(
  [string]$Source = 'C:\Windows\Fonts\NotoSansSC-VF.ttf'
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$py = 'C:\Users\Administrator\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
if (-not (Test-Path $py)) { $py = 'python' }

# 1) collect characters: non-ASCII from the source + full ASCII
$src = [System.IO.File]::ReadAllText((Join-Path $here 'ProjectorUi.cs'))
$set = New-Object System.Collections.Generic.HashSet[string]
foreach ($ch in $src.ToCharArray()) { if ([int]$ch -gt 127) { [void]$set.Add([string]$ch) } }
foreach ($ch in " !`"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\]^_`abcdefghijklmnopqrstuvwxyz{|}~".ToCharArray()) {
  [void]$set.Add([string]$ch)
}
$charsFile = Join-Path $env:TEMP 'ui_chars.txt'
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($charsFile, (($set | Sort-Object) -join ''), $utf8NoBom)
Write-Host "characters: $($set.Count)"

# 2) instantiate wght=600
& $py -m fontTools.varLib.instancer $Source wght=600 -o (Join-Path $env:TEMP 'inst600.ttf')

# 3) subset
New-Item -ItemType Directory -Force -Path (Join-Path $here 'fonts') | Out-Null
& $py -m fontTools.subset (Join-Path $env:TEMP 'inst600.ttf') `
  "--text-file=$charsFile" --output-file (Join-Path $env:TEMP 'ui_subset.ttf') `
  --layout-features='*' --glyph-names --notdef-glyph --notdef-outline --recommended-glyphs `
  --name-IDs='*' --name-legacy --name-languages='*'

# 4) rename family to "XL UI"
$ren = @'
import sys
from fontTools.ttLib import TTFont
src, dst = sys.argv[1], sys.argv[2]
f = TTFont(src)
names = {1: "XL UI", 4: "XL UI Regular", 6: "XLUI-Regular", 16: "XL UI", 17: "Regular"}
for rec in f["name"].names:
    if rec.nameID in names:
        rec.string = names[rec.nameID]
f.save(dst)
'@
$renFile = Join-Path $env:TEMP 'rename_font.py'
[System.IO.File]::WriteAllText($renFile, $ren, $utf8NoBom)
& $py $renFile (Join-Path $env:TEMP 'ui_subset.ttf') (Join-Path $here 'fonts\XLUI.ttf')

$out = Get-Item (Join-Path $here 'fonts\XLUI.ttf')
Write-Host "OK: $($out.FullName)  $([math]::Round($out.Length/1024)) KB"
