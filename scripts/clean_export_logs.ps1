# clean_export_logs.ps1 -- delete Creo interface export logs (*__out.log / *__out.log.N)
# dry-run by default; pass -Delete to really delete.
#   powershell -NoProfile -ExecutionPolicy Bypass -File clean_export_logs.ps1 -Path "D:\J26060131\X6 2P 0810","C:\Users\J26060131\Desktop\板框图" -Recurse
#   ... -Delete
param(
  [Parameter(Mandatory=$true)][string[]]$Path,
  [switch]$Recurse,
  [switch]$Delete
)
$ErrorActionPreference = 'SilentlyContinue'
$total = 0
foreach ($p in $Path) {
  if (-not (Test-Path -LiteralPath $p)) { Write-Output ("skip (not found): " + $p); continue }
  $items = @()
  foreach ($pat in @('*__out.log', '*__out.log.*')) {
    $items += Get-ChildItem -LiteralPath $p -Filter $pat -File -Force -Recurse:$Recurse -ErrorAction SilentlyContinue
  }
  $items = $items | Sort-Object FullName -Unique
  Write-Output ("[" + $p + "]  matched " + @($items).Count + " file(s)")
  foreach ($f in $items) {
    $total++
    if ($Delete) {
      Remove-Item -LiteralPath $f.FullName -Force -ErrorAction SilentlyContinue
      Write-Output ("  deleted: " + $f.FullName)
    } else {
      Write-Output ("  dry-run: " + $f.FullName + "  (" + $f.Length + " bytes, " + $f.LastWriteTime + ")")
    }
  }
}
Write-Output ("total " + $total + " file(s); Delete=" + [bool]$Delete)
