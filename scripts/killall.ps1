# Kill all Creo session processes and the JLinkServer JVM, then verify clean
Get-CimInstance Win32_Process -Filter "Name='xtop.exe' or Name='nmsd.exe' or Name='pro_comm_msg.exe'" |
    ForEach-Object { taskkill /PID $_.ProcessId /T /F 2>&1 | Out-Null }
Get-CimInstance Win32_Process -Filter "Name='java.exe'" |
    Where-Object { $_.CommandLine -match 'JLinkServer' } |
    ForEach-Object { taskkill /PID $_.ProcessId /T /F 2>&1 | Out-Null }
Start-Sleep -Seconds 3
$remaining = Get-CimInstance Win32_Process -Filter "Name='xtop.exe' or Name='nmsd.exe' or Name='pro_comm_msg.exe'"
if ($remaining) { $remaining | ForEach-Object { Write-Host "LEFTOVER: $($_.Name) $($_.ProcessId)" } }
else { Write-Host "CLEAN_NOW" }
$listen = Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
    Where-Object { $_.LocalPort -in 8080, 1239 }
if ($listen) { $listen | Select-Object LocalPort, OwningProcess | Format-Table -AutoSize }
else { Write-Host "PORTS_FREE" }
