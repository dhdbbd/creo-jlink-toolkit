# spawn_session.ps1 -- spawn nmsd + headless xtop for the J-Link service.
# Called by CreoLink.java (spawnViaPs1):
#   powershell -NoProfile -ExecutionPolicy Bypass -File spawn_session.ps1 <nmsdExe> <xtopExe> <objDir> <logDir>
# Contract:
#   - inherits Creo env vars (PRO_DIRECTORY, PTC_D_LICENSE_FILE, ...) from the service JVM
#   - on success writes <logDir>\session.pid with lines:  nmsd=<pid>  /  xtop=<pid>
#   - exit 0 only after xtop has a live pro_comm_msg child
# Why PowerShell Start-Process: java ProcessBuilder spawns of xtop produce a
# permanently-busy (XToolkitBusy) session on Creo 9.0.8.0 -- this path is proven.
param(
  [Parameter(Mandatory=$true)][string]$NmsdExe,
  [Parameter(Mandatory=$true)][string]$XtopExe,
  [Parameter(Mandatory=$true)][string]$ObjDir,
  [Parameter(Mandatory=$true)][string]$LogDir
)
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $NmsdExe)) { throw ("missing: " + $NmsdExe) }
if (-not (Test-Path -LiteralPath $XtopExe)) { throw ("missing: " + $XtopExe) }
if (-not (Test-Path -LiteralPath $LogDir))  { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

$pidFile = Join-Path $LogDir 'session.pid'
if (Test-Path -LiteralPath $pidFile) { Remove-Item -LiteralPath $pidFile -Force }

Write-Output ("[spawn] nmsd: " + $NmsdExe)
$n = Start-Process -FilePath $NmsdExe -ArgumentList '-noservice','-timeout','86400' `
     -RedirectStandardOutput (Join-Path $LogDir 'session_nmsd.log') `
     -RedirectStandardError  (Join-Path $LogDir 'session_nmsd_err.log') `
     -WindowStyle Hidden -PassThru
Write-Output ("[spawn] nmsd pid=" + $n.Id)
Start-Sleep -Seconds 3

Write-Output ("[spawn] xtop: " + $XtopExe)
$x = Start-Process -FilePath $XtopExe -ArgumentList '-g:no_graphics','-i:rpc_input' `
     -WorkingDirectory $ObjDir `
     -RedirectStandardOutput (Join-Path $LogDir 'session_xtop.log') `
     -RedirectStandardError  (Join-Path $LogDir 'session_xtop_err.log') `
     -WindowStyle Hidden -PassThru
Write-Output ("[spawn] xtop pid=" + $x.Id)

# Poll until xtop settles: process alive + child pro_comm_msg exists (max 120s)
$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline) {
    $alive = Get-Process -Id $x.Id -ErrorAction SilentlyContinue
    if (-not $alive) {
        Write-Output ("[spawn] ERROR: xtop pid " + $x.Id + " exited early")
        exit 1
    }
    $child = Get-CimInstance Win32_Process -Filter "Name='pro_comm_msg.exe'" -ErrorAction SilentlyContinue |
             Where-Object { $_.ParentProcessId -eq $x.Id }
    if ($child) {
        $mem = [math]::Round($alive.WorkingSet64 / 1MB, 0)
        Write-Output ("[spawn] xtop READY: pid=" + $x.Id + " child_pro_comm=" + $child.ProcessId + " mem=" + $mem + "MB")
        $lines = "nmsd=" + $n.Id + "`r`n" + "xtop=" + $x.Id + "`r`n"
        [System.IO.File]::WriteAllText($pidFile, $lines)
        exit 0
    }
    Start-Sleep -Seconds 5
}
Write-Output "[spawn] TIMEOUT: no pro_comm_msg child within 120s"
exit 1
