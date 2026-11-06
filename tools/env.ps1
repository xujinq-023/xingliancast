# projector unified build env (PS 5.1 safe: ASCII only). Usage: . D:\DSH-Workplace\projector\tools\env.ps1
$env:JAVA_HOME    = 'C:\Program Files\Microsoft\jdk-17.0.21.21-hotspot'
$env:ANDROID_HOME = 'D:\Dev\Android\Sdk'
$env:Path = "$env:JAVA_HOME\bin;D:\Dev\Android\Sdk\platform-tools;D:\Dev\Android\Sdk\build-tools\34.0.0;D:\Dev\Android\cmdline-tools\latest\bin;D:\Dev\tools\jadx\bin;$env:Path"
# Import VS BuildTools (MSVC/CMake/SDK) env into current session (batch route, most reliable)
$vsenv_bat = 'D:\DSH-Workplace\projector\tools\vsenv.bat'
if (Test-Path $vsenv_bat) {
  $lines = & cmd /c $vsenv_bat
  foreach ($l in $lines) {
    if ($l -match '^([^=]+)=(.*)$') { Set-Item -Path ('env:' + $matches[1]) -Value $matches[2] }
  }
}
