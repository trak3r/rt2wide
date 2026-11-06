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

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Applies [{Key, Name, Value}] compatibility changes. HKCU is written directly; HKLM needs admin,
# so unless we already are, a single elevated helper is started (one UAC prompt).
# Returns $false if the user declined the prompt or the helper failed.
function Set-CompatValues($items) {
    $items = @($items)
    $user = @($items | Where-Object { $_.Key -like 'HKCU:*' })
    $machine = @($items | Where-Object { $_.Key -like 'HKLM:*' })
    $all = @()
    if ($user.Count) { $all += , $user }
    if ($machine.Count) { $all += , $machine }
    foreach ($group in $all) {
        $json = Join-Path $env:TEMP ("rt2wide-compat-{0}.json" -f [guid]::NewGuid())
        ConvertTo-Json -InputObject @($group) | Set-Content $json
        try {
            $helper = Join-Path $PSScriptRoot 'set-compat.ps1'
            if ($group[0].Key -like 'HKCU:*' -or (Test-IsAdmin)) {
                & $helper -Json $json
            } else {
                Write-Warn2 'Windows will ask for permission to change the machine-wide setting (UAC).'
                $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$helper`" -Json `"$json`""
                try { $p = Start-Process powershell -ArgumentList $argList -Verb RunAs -Wait -PassThru -WindowStyle Hidden }
                catch { Write-Warn2 'Permission was declined; the machine-wide setting was not changed.'; return $false }
                if ($p.ExitCode -ne 0) { Write-Warn2 "The elevated helper failed (exit $($p.ExitCode))."; return $false }
                foreach ($i in $group) { Write-Ok "set $($i.Key)" }
            }
        } finally { Remove-Item $json -ErrorAction SilentlyContinue }
    }
    $true
}
