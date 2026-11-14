' Projector sender launcher (logon autostart): starts sender.exe with no console window.
' No shell redirection: cmd /c with a quoted ">> path" breaks under WScript.Shell.Run,
' and sender.exe will write its own log file (see --log option / file logger).
' ASCII only (Windows Script Host reads BOM-less files as ANSI).
Option Explicit

Dim fso, sh, base, exePath, args, cfg, f, line
Set fso = CreateObject("Scripting.FileSystemObject")
Set sh  = CreateObject("WScript.Shell")

base    = fso.GetParentFolderName(WScript.ScriptFullName)
exePath = base & "\sender.exe"
If Not fso.FileExists(exePath) Then exePath = base & "\build\sender.exe"   ' dev layout: exe is in build\

' ---- args from autostart.cfg (line: ARGS=--height 1440 --display 0) ----
args = "--height 1440"
cfg  = base & "\autostart.cfg"
If fso.FileExists(cfg) Then
  Set f = fso.OpenTextFile(cfg, 1)
  Do Until f.AtEndOfStream
    line = Trim(f.ReadLine)
    If InStr(1, line, "ARGS=", vbTextCompare) = 1 Then args = Trim(Mid(line, 6))
  Loop
  f.Close
End If

If Not fso.FileExists(exePath) Then
  MsgBox "sender.exe not found: " & exePath, 48, "Projector"
  WScript.Quit 1
End If

sh.CurrentDirectory = base & "\build"
sh.Run """" & exePath & """ " & args, 0, False
