$ErrorActionPreference = 'Stop'

$Root      = Split-Path $PSScriptRoot -Parent
$GameDir   = if ($env:RT2_GAME_DIR) { $env:RT2_GAME_DIR } else { 'C:\Program Files (x86)\GOG Galaxy\Games\Railroad Tycoon II' }
$GameExe   = Join-Path $GameDir 'RT2_PLAT.EXE'
$BackupDir = Join-Path $Root 'backup\original'
$BackupFiles    = Join-Path $BackupDir 'files'
$ManifestPath   = Join-Path $BackupDir 'manifest.csv'
$CompatPath     = Join-Path $BackupDir 'compat.json'
$BuildDir  = Join-Path $Root 'build'

# SHA-256 of the GOG Railroad Tycoon II Platinum executable this mod is written against.
$KnownExeHash = '510B9E449612E5C6619D0B86CB65C453CEFD26370065F1FD77865760D017E474'

# Every file the mod may ever place in the game folder. Rollback removes these
# (only if they were not part of the original install).
$ModFiles = @('ddraw.dll', 'rt2wide.ini', 'rt2wide.log')

$CompatKeys = @(
    'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers',
    'HKLM:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers'
)

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Ok($msg)   { Write-Host "    $msg" -ForegroundColor Green }
function Write-Warn2($msg) { Write-Host "    $msg" -ForegroundColor Yellow }

function Assert-GameNotRunning {
    $p = Get-Process -Name 'RT2_PLAT' -ErrorAction SilentlyContinue
    if ($p) { throw 'Railroad Tycoon II is running. Quit the game first, then run this again.' }
}

function Get-RelativePath([string]$base, [string]$full) {
    $full.Substring($base.TrimEnd('\').Length + 1)
}

function Get-FolderManifest([string]$dir) {
    Get-ChildItem -LiteralPath $dir -Recurse -File -Force | ForEach-Object {
        [pscustomobject]@{
            Path   = Get-RelativePath $dir $_.FullName
            Length = $_.Length
            Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        }
    }
}

function Get-CompatValues {
    $result = @()
    foreach ($key in $CompatKeys) {
        if (-not (Test-Path $key)) { continue }
        $props = (Get-ItemProperty $key).PSObject.Properties |
            Where-Object { $_.Name -like "$GameDir\*" }
        foreach ($p in $props) { $result += [pscustomobject]@{ Key = $key; Name = $p.Name; Value = $p.Value } }
    }
    $result
}

function Test-IsSave([string]$relPath) {
    $relPath -like 'games\*'
}
