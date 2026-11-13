# Manually spawn nmsd + headless xtop (same env as run_server.bat, no J-Link client involved)
$ErrorActionPreference = 'Stop'

$creoHome  = '__CREO_HOME__'
$lic       = '__LICENSE_FILE__'
$pkg       = (Split-Path -Parent $PSScriptRoot)
$common    = "$creoHome\Common Files"
$creoLib   = "$common\x86e_win64\lib"
$creoObj   = "$common\x86e_win64\obj"
$logDir    = Join-Path $pkg 'jlink\service\logs'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }

$env:PTC_D_LICENSE_FILE      = $lic
$env:PRO_COMM_MSG_EXE        = "$creoObj\pro_comm_msg.exe"
$env:PRO_DIRECTORY           = $common
$env:PRO_MACHINE_TYPE        = 'x86e_win64'
$env:CREOAPP_DIRECTORY       = "$creoHome\Parametric"
$env:CREOPMA_FEATURE_NAME    = '__FEATURE_NAME__'
$env:PVIEW_PROEPVIEW_HOME    = "$common\apps\prodview"
$env:MECH_LP                 = "$common\applications\simulate"
$env:MECH_HOME               = "$env:MECH_LP\x86e_win64"
$env:PATH                    = "$pkg\jlink\native;$creoLib;$pkg\jlink\runtime\bin;$env:PATH"

$nmsdExe = "$common\x86e_win64\nms\nmsd.exe"
$xtopExe = "$creoObj\xtop.exe"

Write-Host "[spawn] nmsd: $nmsdExe"
$n = Start-Process -FilePath $nmsdExe -ArgumentList '-noservice','-timeout','86400' `
     -RedirectStandardOutput "$logDir\manual_nmsd.log" -RedirectStandardError "$logDir\manual_nmsd_err.log" `
     -WindowStyle Hidden -PassThru
Write-Host "[spawn] nmsd pid=$($n.Id)"
Start-Sleep -Seconds 3

Write-Host "[spawn] xtop: $xtopExe"
$x = Start-Process -FilePath $xtopExe -ArgumentList '-g:no_graphics','-i:rpc_input' `
     -WorkingDirectory $creoObj `
     -RedirectStandardOutput "$logDir\manual_xtop.log" -RedirectStandardError "$logDir\manual_xtop_err.log" `
     -WindowStyle Hidden -PassThru
Write-Host "[spawn] xtop pid=$($x.Id)"

# Poll until xtop is settled: alive + child pro_comm_msg exists (max 120s)
$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline) {
    $alive = Get-Process -Id $x.Id -ErrorAction SilentlyContinue
    if (-not $alive) { Write-Host "[spawn] ERROR: xtop pid $($x.Id) exited early"; exit 1 }
    $child = Get-CimInstance Win32_Process -Filter "Name='pro_comm_msg.exe'" -ErrorAction SilentlyContinue |
             Where-Object { $_.ParentProcessId -eq $x.Id }
    if ($child) {
        $mem = [math]::Round($alive.WorkingSet64 / 1MB, 0)
        Write-Host "[spawn] xtop READY: pid=$($x.Id) child_pro_comm=$($child.ProcessId) mem=${mem}MB"
        exit 0
    }
    Start-Sleep -Seconds 5
}
Write-Host "[spawn] TIMEOUT: no pro_comm_msg child within 120s"
exit 1
