' Hidden runner for arbitrary command lines (no console window)
' Usage: wscript.exe run_hidden.vbs "cmd /c xxx.bat"  or  wscript.exe run_hidden.vbs "powershell -File xxx.ps1"
Dim cmd, sh
If WScript.Arguments.Count > 0 Then
    cmd = WScript.Arguments(0)
Else
    WScript.Echo "usage: wscript run_hidden.vbs ""<command line>"""
    WScript.Quit 1
End If
Set sh = CreateObject("WScript.Shell")
sh.Run cmd, 0, False
