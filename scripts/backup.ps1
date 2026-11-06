# Takes a one-time snapshot of the untouched game folder. Never overwrites an existing snapshot.
param([switch]$AllowUnknownExe)
. "$PSScriptRoot\common.ps1"

if (Test-Path $ManifestPath) {
    Write-Ok "Original backup already exists at $BackupDir (left untouched)."
    return
}

Write-Step "Checking the game folder is in its original state"
if (-not (Test-Path $GameExe)) { throw "Game not found at $GameExe" }
Assert-GameNotRunning
foreach ($f in $ModFiles) {
    if (Test-Path (Join-Path $GameDir $f)) {
        throw "$f already exists in the game folder, so it is not in its original state. Remove it (or repair the game in GOG Galaxy) before taking the backup."
    }
}
$exeHash = (Get-FileHash $GameExe -Algorithm SHA256).Hash
if ($exeHash -ne $KnownExeHash -and -not $AllowUnknownExe) {
    throw "RT2_PLAT.EXE is not the expected GOG build (hash $exeHash). Re-run with -AllowUnknownExe to back it up anyway."
}
Write-Ok "RT2_PLAT.EXE matches the GOG build."

Write-Step "Copying the whole game folder to $BackupFiles"
New-Item -ItemType Directory -Force $BackupFiles | Out-Null
& robocopy $GameDir $BackupFiles /E /COPY:DAT /DCOPY:DAT /R:1 /W:1 /NP /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed with exit code $LASTEXITCODE" }

Write-Step "Fingerprinting every original file"
$manifest = @(Get-FolderManifest $GameDir)
$copy = @{}
Get-FolderManifest $BackupFiles | ForEach-Object { $copy[$_.Path] = $_.Sha256 }
$bad = $manifest | Where-Object { $copy[$_.Path] -ne $_.Sha256 }
if ($bad) { throw "Backup copy does not match the game folder: $($bad.Path -join ', ')" }
$manifest | Export-Csv -NoTypeInformation -Path $ManifestPath
Write-Ok "$($manifest.Count) files copied and verified."

Write-Step "Recording Windows compatibility settings"
@(Get-CompatValues) | ConvertTo-Json | Set-Content $CompatPath
[pscustomobject]@{
    Created  = (Get-Date).ToString('o')
    GameDir  = $GameDir
    ExeHash  = $exeHash
} | ConvertTo-Json | Set-Content (Join-Path $BackupDir 'info.json')

Write-Ok "Backup complete: $BackupDir"
