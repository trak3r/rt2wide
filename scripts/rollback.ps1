# Returns the game folder to the original backed-up state.
#   -Check         report differences only, change nothing
#   -RestoreSaves  also revert the original save files in games\ (off by default so progress is never lost)
param([switch]$Check, [switch]$RestoreSaves)
. "$PSScriptRoot\common.ps1"

if (-not (Test-Path $ManifestPath)) { throw "No original backup found at $BackupDir, so there is nothing to roll back to." }
if (-not $Check) { Assert-GameNotRunning }

$manifest = Import-Csv $ManifestPath
$original = @{}
$manifest | ForEach-Object { $original[$_.Path] = $_ }
$problems = 0

Write-Step "Mod files"
foreach ($f in $ModFiles) {
    $p = Join-Path $GameDir $f
    if ((Test-Path $p) -and -not $original.ContainsKey($f)) {
        if ($Check) { Write-Warn2 "present: $f"; $problems++ }
        else { Remove-Item -LiteralPath $p -Force; Write-Ok "removed $f" }
    }
}

Write-Step "Original files"
foreach ($entry in $manifest) {
    $dest = Join-Path $GameDir $entry.Path
    $src  = Join-Path $BackupFiles $entry.Path
    $state = if (-not (Test-Path -LiteralPath $dest)) { 'missing' }
             elseif ((Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash -ne $entry.Sha256) { 'changed' }
             else { 'ok' }
    if ($state -eq 'ok') { continue }

    if ($state -eq 'changed' -and (Test-IsSave $entry.Path) -and -not $RestoreSaves) {
        Write-Ok "kept your changed save: $($entry.Path)"
        continue
    }
    if ($Check) { Write-Warn2 "${state}: $($entry.Path)"; $problems++; continue }

    New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
    Copy-Item -LiteralPath $src -Destination $dest -Force
    Write-Ok "restored ($state): $($entry.Path)"
}

Write-Step "Files added since the backup (left alone)"
$extra = Get-ChildItem -LiteralPath $GameDir -Recurse -File -Force |
    ForEach-Object { Get-RelativePath $GameDir $_.FullName } |
    Where-Object { -not $original.ContainsKey($_) -and $ModFiles -notcontains $_ }
if ($extra) { $extra | ForEach-Object { Write-Ok $_ } } else { Write-Ok 'none' }

Write-Step "Windows compatibility settings"
$saved = @()
if (Test-Path $CompatPath) { $saved = @(Get-Content $CompatPath -Raw | ConvertFrom-Json | ForEach-Object { $_ }) }
$now = @(Get-CompatValues)
$compatChanges = @()
foreach ($s in $saved) {
    $cur = $now | Where-Object { $_.Key -eq $s.Key -and $_.Name -eq $s.Name }
    if ($cur.Value -eq $s.Value) { continue }
    if ($Check) { Write-Warn2 "differs: $($s.Key) -> '$($cur.Value)' (original '$($s.Value)')"; $problems++; continue }
    $compatChanges += [pscustomobject]@{ Key = $s.Key; Name = $s.Name; Value = $s.Value }
}
foreach ($c in $now) {
    if ($saved | Where-Object { $_.Key -eq $c.Key -and $_.Name -eq $c.Name }) { continue }
    if ($Check) { Write-Warn2 "added since backup: $($c.Key) = '$($c.Value)'"; $problems++; continue }
    $compatChanges += [pscustomobject]@{ Key = $c.Key; Name = $c.Name; Value = $null }
}
if ($compatChanges.Count) {
    if (-not (Set-CompatValues $compatChanges)) {
        Write-Warn2 'Run ROLLBACK.cmd again and allow the prompt to finish restoring the machine-wide setting.'
    }
} elseif (-not $Check) { Write-Ok 'already original' }

if ($Check) {
    if ($problems -eq 0) { Write-Host "`nGame matches the original backup." -ForegroundColor Green }
    else { Write-Host "`n$problems difference(s) from the original. Run ROLLBACK.cmd to restore." -ForegroundColor Yellow }
} else {
    Write-Host "`nRolled back. The game is back to its original state." -ForegroundColor Green
}
