param(
  [string]$CreoHome = '',
  [string]$LicenseFile = '',
  [string]$FeatureName = 'creopma',
  [switch]$Register
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot

if (-not $CreoHome) {
  $found = Get-ChildItem 'C:\Program Files\PTC' -Directory -Filter 'Creo*' -ErrorAction SilentlyContinue |
           Sort-Object Name -Descending | Select-Object -First 1
  if ($found) { $CreoHome = $found.FullName }
}
if (-not $CreoHome) { throw '找不到 Creo。请用 -CreoHome "<Creo 安装目录>" 指定。' }
if (Test-Path -LiteralPath $CreoHome) {
  $CreoHome = (Resolve-Path -LiteralPath $CreoHome).Path.TrimEnd('\')
}
if (-not (Test-Path (Join-Path $CreoHome 'Common Files'))) {
  throw "这个目录不像 Creo loadpoint（没有 Common Files）：$CreoHome"
}
if (-not (Test-Path (Join-Path $CreoHome 'Common Files\x86e_win64\obj\xtop.exe'))) {
  Write-Warning "没找到 xtop.exe，确认这是 64 位 Creo 9 的 loadpoint：$CreoHome"
}

if (-not $LicenseFile) { $LicenseFile = $env:PTC_D_LICENSE_FILE }
if (-not $LicenseFile) {
  $LicenseFile = 'C:\Path\To\Your\License.dat'
  Write-Warning '没给授权。已写入占位路径，运行前请用 -LicenseFile 重新执行，或先设 PTC_D_LICENSE_FILE。'
}

$map = [ordered]@{
  '__PACKAGE_ROOT__' = $Root
  '__CREO_HOME__'    = $CreoHome
  '__LICENSE_FILE__' = $LicenseFile
  '__FEATURE_NAME__' = $FeatureName
}

$exts = @('.bat', '.cmd', '.ps1', '.vbs', '.dat', '.pro', '.txt')
$changed = 0
Get-ChildItem $Root -Recurse -File | Where-Object {
  $exts -contains $_.Extension.ToLower() -and $_.FullName -ne $PSCommandPath
} | ForEach-Object {
  $text = [IO.File]::ReadAllText($_.FullName)
  $before = $text
  foreach ($k in $map.Keys) { $text = $text.Replace($k, $map[$k]) }
  if ($text -ne $before) {
    [IO.File]::WriteAllText($_.FullName, $text, (New-Object System.Text.UTF8Encoding($false)))
    $changed++
  }
}

# 工作目录
foreach ($d in @('out', 'work\logs', 'work\trail', 'work-asmflat\trail', 'jlink\service\logs')) {
  $p = Join-Path $Root $d
  if (-not (Test-Path $p)) { New-Item -ItemType Directory -Path $p -Force | Out-Null }
}

# 可选：把注册项写进用户级 config.pro
$cfgLine = 'toolkit_registry_file ' + (Join-Path $Root 'work\protk.dat')
if ($Register) {
  $cfg = Join-Path $env:HOMEDRIVE 'config.pro'
  if (-not (Test-Path $cfg)) { New-Item -ItemType File -Path $cfg -Force | Out-Null }
  $hit = Select-String -Path $cfg -Pattern 'toolkit_registry_file' -SimpleMatch -Quiet
  if (-not $hit) { Add-Content -Path $cfg -Value $cfgLine -Encoding ASCII }
  Write-Output ('config.pro: ' + $cfg + $(if ($hit) { '（已有一行，未重复写入）' } else { '（已追加）' }))
}

$left = Get-ChildItem $Root -Recurse -File |
  Where-Object { $exts -contains $_.Extension.ToLower() -and $_.FullName -ne $PSCommandPath } |
  Select-String -Pattern '__[A-Z_]+__' -SimpleMatch -ErrorAction SilentlyContinue

Write-Output ''
Write-Output ('包目录   : ' + $Root)
Write-Output ('Creo     : ' + $CreoHome)
Write-Output ('授权     : ' + $LicenseFile)
Write-Output ('Feature  : ' + $FeatureName)
Write-Output ('改写文件 : ' + $changed)
if ($left) {
  Write-Warning '还有未替换的占位符：'
  $left | ForEach-Object { Write-Output ('  ' + $_.Path + ':' + $_.LineNumber) }
} else {
  Write-Output '占位符   : 全部替换完成'
}
if (-not $Register) {
  Write-Output ''
  Write-Output '下一步：把下面这行加进 Creo 用户级 config.pro（或重新运行本脚本加 -Register）：'
  Write-Output ('  ' + $cfgLine)
}
