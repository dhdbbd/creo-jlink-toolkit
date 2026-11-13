param(
  [string]$Work = (Join-Path $PSScriptRoot '..\work'),
  [int]$TimeoutSec = 900,
  [switch]$KeepOpen
)
$ErrorActionPreference = 'Stop'

$common = Join-Path '__CREO_HOME__' 'Common Files'
$nmsd = Join-Path $common 'x86e_win64\nms\nmsd.exe'
$xtop = Join-Path $common 'x86e_win64\obj\xtop.exe'
$lib  = Join-Path $common 'x86e_win64\lib'
$obj  = Join-Path $common 'x86e_win64\obj'
$logDir = Join-Path $Work 'logs'
if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }

foreach ($p in @($nmsd, $xtop)) { if (-not (Test-Path -LiteralPath $p)) { throw ('missing: ' + $p) } }

# --- environment (same set that works for the J-Link headless spawn) ---
$env:PRO_DIRECTORY     = $common
$env:PRO_MACHINE_TYPE  = 'x86e_win64'
$env:PRO_COMM_MSG_EXE  = Join-Path $obj 'pro_comm_msg.exe'
$env:CREOAPP_DIRECTORY = Join-Path '__CREO_HOME__' 'Parametric'
$env:CREOPMA_FEATURE_NAME = '__FEATURE_NAME__'
$env:PVIEW_PROEPVIEW_HOME = Join-Path $common 'apps\prodview'
$env:MECH_LP           = Join-Path $common 'applications\simulate'
$env:MECH_HOME         = (Join-Path $env:MECH_LP 'x86e_win64')
$env:PTC_D_LICENSE_FILE = '__LICENSE_FILE__'
$env:PATH = ($lib + ';' + $obj + ';' + $env:PATH)
Get-ChildItem env: | Where-Object { $_.Name -like 'BTK_*' } | ForEach-Object { Remove-Item ('env:' + $_.Name) -ErrorAction SilentlyContinue }

# --- clean previous run artifacts ---
$nOut = Join-Path $logDir 'nmsd_out.log'; $nErr = Join-Path $logDir 'nmsd_err.log'
$xOut = Join-Path $logDir 'xtop_out.log'; $xErr = Join-Path $logDir 'xtop_err.log'
$runLog = Join-Path $Work 'sync_export_run.log'
foreach ($f in @($nOut, $nErr, $xOut, $xErr, $runLog)) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue } }

Write-Output ('[' + (Get-Date -Format 'HH:mm:ss') + '] starting nmsd ...')
$pNmsd = Start-Process -FilePath $nmsd -ArgumentList '-noservice','-timeout','86400' -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput $nOut -RedirectStandardError $nErr
Start-Sleep -Seconds 3
Write-Output ('[' + (Get-Date -Format 'HH:mm:ss') + '] nmsd pid=' + $pNmsd.Id)

Write-Output ('[' + (Get-Date -Format 'HH:mm:ss') + '] starting xtop (cwd=' + $Work + ') ...')
$pXtop = Start-Process -FilePath $xtop -ArgumentList '-g:no_graphics','-i:rpc_input' -WorkingDirectory $Work -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput $xOut -RedirectStandardError $xErr
Write-Output ('[' + (Get-Date -Format 'HH:mm:ss') + '] xtop pid=' + $pXtop.Id)
[System.IO.File]::WriteAllText((Join-Path $logDir 'pids.txt'), ('nmsd=' + $pNmsd.Id + "`r`nxtop=" + $pXtop.Id + "`r`n"), (New-Object System.Text.UTF8Encoding($false)))

function Read-Shared([string]$path) {
  if (-not (Test-Path -LiteralPath $path)) { return '' }
  try {
    $fs = New-Object System.IO.FileStream($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    $sr = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)
    $t = $sr.ReadToEnd()
    $sr.Close(); $fs.Close()
    return $t
  } catch { return '' }
}

# --- poll for completion marker ---
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$done = $false
while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
  Start-Sleep -Seconds 5
  if ($pXtop.HasExited) { Write-Output ('[!] xtop exited early, code=' + $pXtop.ExitCode); break }
  $tail = Read-Shared $xErr
  if ($tail -and $tail.Contains('SYNC_EXPORT_DONE')) { $done = $true; break }
  if ([int]$sw.Elapsed.TotalSeconds % 30 -lt 5) { Write-Output ('    ... ' + [int]$sw.Elapsed.TotalSeconds + 's') }
}

Write-Output ('[' + (Get-Date -Format 'HH:mm:ss') + '] done=' + $done + ' elapsed=' + [int]$sw.Elapsed.TotalSeconds + 's')

Write-Output '=== sync_export_run.log ==='
if (Test-Path -LiteralPath $runLog) { Get-Content -LiteralPath $runLog -Encoding UTF8 | ForEach-Object { Write-Output ('  ' + $_) } } else { Write-Output '  (no run log)' }

Write-Output '=== xtop_err tail (last 15 lines) ==='
if (Test-Path -LiteralPath $xErr) { Get-Content -LiteralPath $xErr -Tail 15 -Encoding UTF8 | ForEach-Object { Write-Output ('  ' + $_) } }

Write-Output '=== exported files ==='
Get-ChildItem -LiteralPath (Join-Path $Work '..\out') -ErrorAction SilentlyContinue |
  Select-Object Name, Length, LastWriteTime | ForEach-Object { Write-Output ('  ' + $_.Name + '  ' + $_.Length + '  ' + $_.LastWriteTime) }

if (-not $KeepOpen) {
  Write-Output ('[' + (Get-Date -Format 'HH:mm:ss') + '] stopping our session (pid ' + $pXtop.Id + ' / ' + $pNmsd.Id + ')')
  cmd /c ("taskkill /PID " + $pXtop.Id + " /T /F >nul 2>&1")
  cmd /c ("taskkill /PID " + $pNmsd.Id + " /T /F >nul 2>&1")
  Write-Output 'stopped.'
} else {
  Write-Output ('[i] left running: xtop=' + $pXtop.Id + ' nmsd=' + $pNmsd.Id)
}
