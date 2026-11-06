# Adds the widescreen wrapper to the game folder. Only adds files; never modifies original ones.
param([switch]$AllowUnknownExe, [switch]$ResetConfig)
. "$PSScriptRoot\common.ps1"

Assert-GameNotRunning
& "$PSScriptRoot\backup.ps1" -AllowUnknownExe:$AllowUnknownExe

$exeHash = (Get-FileHash $GameExe -Algorithm SHA256).Hash
if ($exeHash -ne $KnownExeHash -and -not $AllowUnknownExe) {
    throw "RT2_PLAT.EXE is not the GOG build this mod was made for (hash $exeHash). Not installing."
}

$dll = Join-Path $BuildDir 'ddraw.dll'
$ini = Join-Path $Root 'config\rt2wide.ini'
if (-not (Test-Path $dll)) { throw "Build output $dll not found. Run BUILD.cmd first." }

$original = @{}
Import-Csv $ManifestPath | ForEach-Object { $original[$_.Path] = $true }
foreach ($f in 'ddraw.dll', 'rt2wide.ini') {
    if ($original.ContainsKey($f)) { throw "$f was part of the original game; refusing to overwrite it." }
}

Write-Step "Installing into $GameDir"
Copy-Item $dll (Join-Path $GameDir 'ddraw.dll') -Force
Write-Ok 'ddraw.dll'
$gameIni = Join-Path $GameDir 'rt2wide.ini'
if ($ResetConfig -or -not (Test-Path $gameIni)) {
    Copy-Item $ini $gameIni -Force
    Write-Ok 'rt2wide.ini'
} else {
    Write-Ok 'rt2wide.ini (kept your existing settings)'
}

Write-Host "`nInstalled. Launch the game as usual. To undo everything, run ROLLBACK.cmd." -ForegroundColor Green
