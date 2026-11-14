# receiver-tv M2 build - PowerShell 5.1 compatible (no &&, one statement per line).
# Pipeline: aapt2 compile/link -> javac -> jar -> d8 -> inject dex -> zipalign -> keytool -> apksigner.
# Output: receiver-tv\out\receiver-tv-debug.apk
# ASCII only (PS 5.1 reads BOM-less files as ANSI).

$ErrorActionPreference = 'Continue'

$Jdk       = 'C:\Program Files\Microsoft\jdk-17.0.21.21-hotspot'
$env:JAVA_HOME = $Jdk
$JdkBin    = Join-Path $Jdk 'bin'
$Sdk       = 'D:\Dev\Android\Sdk'
$Bt        = Join-Path $Sdk 'build-tools\34.0.0'
$AndroidJar = Join-Path $Sdk 'platforms\android-34\android.jar'

$Root  = $PSScriptRoot
$Gen   = Join-Path $Root 'build'
$Out   = Join-Path $Root 'out'
$Keystore = Join-Path $Root 'debug.keystore'

function Invoke-Tool {
    param([string]$Exe, [string[]]$ToolArgs)
    Write-Host ('> ' + $Exe + ' ' + ($ToolArgs -join ' '))
    $output = & $Exe @ToolArgs 2>&1
    $code = $LASTEXITCODE
    if ($output) { $output | ForEach-Object { Write-Host ('  | ' + $_) } }
    if ($code -ne 0) { throw ("tool failed (exit $code): $Exe " + ($ToolArgs -join ' ')) }
    return $output
}

# ---------- 0. clean ----------
if (Test-Path $Gen) { Remove-Item -Recurse -Force $Gen }
New-Item -ItemType Directory -Force -Path $Gen | Out-Null
if (Test-Path $Out) { Remove-Item -Force (Join-Path $Out '*.apk') -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $Out | Out-Null

# ---------- 1. aapt2 compile (res/ if present) ----------
$ResFlags = @()
$ResDir = Join-Path $Root 'res'
if (Test-Path $ResDir) {
    $FlatZip = Join-Path $Gen 'res.zip'
    Invoke-Tool (Join-Path $Bt 'aapt2.exe') @('compile', '--dir', $ResDir, '-o', $FlatZip) | Out-Null
    $ResFlags = @($FlatZip)   # aapt2 link 把编译后的资源 zip 作为位置参数，而不是 --res
}

# ---------- 2. aapt2 link ----------
$BaseApk = Join-Path $Gen 'base.apk'
Invoke-Tool (Join-Path $Bt 'aapt2.exe') (@(
    'link',
    '-o', $BaseApk,
    '-I', $AndroidJar,
    '--manifest', (Join-Path $Root 'AndroidManifest.xml'),
    '--min-sdk-version', '24',
    '--version-code', '1',
    '--version-name', '0.1'
) + $ResFlags) | Out-Null

# ---------- 3. javac ----------
$ClassesDir = Join-Path $Gen 'classes'
New-Item -ItemType Directory -Force -Path $ClassesDir | Out-Null
$SrcDir = Join-Path $Root 'src'
$Srcs = @(Get-ChildItem -Recurse -Filter *.java $SrcDir | ForEach-Object { $_.FullName })
if ($Srcs.Count -eq 0) { throw 'no java sources found under src\' }
Invoke-Tool (Join-Path $JdkBin 'javac.exe') (@(
    '--release', '11',
    '-encoding', 'UTF-8',
    '-classpath', $AndroidJar,
    '-d', $ClassesDir
) + $Srcs) | Out-Null

# ---------- 4. jar ----------
$ClassesJar = Join-Path $Gen 'classes.jar'
Invoke-Tool (Join-Path $JdkBin 'jar.exe') @('--create', '--file', $ClassesJar, '-C', $ClassesDir, '.') | Out-Null

# ---------- 5. d8 (classes.jar -> classes.dex; build-tools 34 writes the dex straight out) ----------
$DexOut = Join-Path $Gen 'dexout'
New-Item -ItemType Directory -Force -Path $DexOut | Out-Null
Invoke-Tool (Join-Path $Bt 'd8.bat') @(
    '--release',
    '--min-api', '24',
    '--lib', $AndroidJar,
    '--output', $DexOut,
    $ClassesJar
) | Out-Null

# ---------- 6. inject classes.dex into the apk ----------
$DexFile = Join-Path $DexOut 'classes.dex'
if (-not (Test-Path $DexFile)) {
    # fallback: some d8 versions emit a jar; extract it
    $DexJar = Join-Path $DexOut 'classes.jar'
    if (-not (Test-Path $DexJar)) { throw 'd8 produced neither dexout\classes.dex nor classes.jar' }
    Push-Location $DexOut
    try {
        Invoke-Tool (Join-Path $JdkBin 'jar.exe') @('--extract', '--file', $DexJar, 'classes.dex') | Out-Null
    } finally { Pop-Location }
}
if (-not (Test-Path $DexFile)) { throw 'classes.dex missing after d8' }

$WithDex = Join-Path $Gen 'unsigned.apk'
Copy-Item $BaseApk $WithDex -Force
Invoke-Tool (Join-Path $JdkBin 'jar.exe') @('--update', '--file', $WithDex, '-C', $DexOut, 'classes.dex') | Out-Null

# ---------- 7. zipalign ----------
$Aligned = Join-Path $Gen 'aligned.apk'
Invoke-Tool (Join-Path $Bt 'zipalign.exe') @('-f', '4', $WithDex, $Aligned) | Out-Null

# ---------- 8. debug keystore (one time) ----------
if (-not (Test-Path $Keystore)) {
    Invoke-Tool (Join-Path $JdkBin 'keytool.exe') @(
        '-genkeypair',
        '-keystore', $Keystore,
        '-alias', 'androiddebugkey',
        '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000',
        '-dname', 'CN=Android Debug,O=Android,C=US',
        '-storepass', 'android', '-keypass', 'android'
    ) | Out-Null
}

# ---------- 9. apksigner sign ----------
$FinalApk = Join-Path $Out 'receiver-tv-debug.apk'
Invoke-Tool (Join-Path $Bt 'apksigner.bat') @(
    'sign',
    '--ks', $Keystore,
    '--ks-pass', 'pass:android',
    '--key-pass', 'pass:android',
    '--out', $FinalApk,
    $Aligned
) | Out-Null

# ---------- 10. verify ----------
Invoke-Tool (Join-Path $Bt 'apksigner.bat') @('verify', '--print-certs', $FinalApk) | Out-Null
$listing = Invoke-Tool (Join-Path $JdkBin 'jar.exe') @('--list', '--file', $FinalApk)
if (-not ($listing | Where-Object { $_ -eq 'classes.dex' })) {
    throw 'classes.dex is NOT inside the final apk'
}

# ---------- 11. host smoke test of the pure-Java core (SPS parse / csd extract) ----------
if (Test-Path (Join-Path $Root 'test\TestCore.java')) {
    $TestOut = Join-Path $Gen 'testout'
    New-Item -ItemType Directory -Force -Path $TestOut | Out-Null
    Invoke-Tool (Join-Path $JdkBin 'javac.exe') @(
        '--release', '11', '-encoding', 'UTF-8',
        '-classpath', "$AndroidJar;$ClassesJar",
        '-d', $TestOut,
        (Join-Path $Root 'test\TestCore.java')
    ) | Out-Null
    Invoke-Tool (Join-Path $JdkBin 'java.exe') @(
        '-cp', "$ClassesJar;$TestOut;$AndroidJar",
        'tv.projector.receiver.TestCore'
    ) | Out-Null
}

Write-Host ''
Write-Host ('BUILD OK: ' + $FinalApk + '  (' + (Get-Item $FinalApk).Length + ' bytes)')
