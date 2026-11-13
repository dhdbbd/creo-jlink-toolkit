param(
  [Parameter(Mandatory=$true)][string]$Out,
  [string]$Base = (Join-Path $PSScriptRoot '..\profiles\ap242.dep_step'),
  [ValidateSet('0','1','2')][string]$Protocol = '2',   # 0=AP203 1=AP214 2=AP242
  [ValidateSet('0','1')][string]$Quilts = '0',         # 0=不导出面组
  [ValidateSet('0','1')][string]$Hidden = '0'          # 0=不导出隐藏图元
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Base)) { throw ('base profile not found: ' + $Base) }
$text = [System.IO.File]::ReadAllText($Base, [System.Text.Encoding]::UTF8)

function Set-ProfileValue([string]$t, [string]$key, [string]$value) {
  $pattern = '(id_="' + [regex]::Escape($key) + '"[^>]*>\s*<value___PO__[a-z]+__PC__>)[^<]*(</value___PO__)'
  $new = [regex]::Replace($t, $pattern, ('${1}' + $value + '${2}'))
  if ($new -eq $t) { throw ('profile key not changed: ' + $key) }
  return $new
}

$text = Set-ProfileValue $text 'Application protocol' $Protocol
$text = Set-ProfileValue $text 'Quilts' $Quilts
$text = Set-ProfileValue $text 'Hidden entities' $Hidden

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
[System.IO.File]::WriteAllText($Out, $text, (New-Object System.Text.UTF8Encoding($false)))
Write-Output ('profile written: ' + $Out + '  (Protocol=' + $Protocol + ' Quilts=' + $Quilts + ' Hidden=' + $Hidden + ')')
