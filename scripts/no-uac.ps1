# Stops the admin (UAC) prompt on every launch. GOG registers the game for Windows XP
# compatibility mode, which makes Windows run it elevated; this removes only that part.
# ROLLBACK.cmd puts the original values back.
. "$PSScriptRoot\common.ps1"

Assert-GameNotRunning
Write-Step 'Windows compatibility settings (removing XP mode, which causes the admin prompt)'
$xpLayers = '^(WINXPSP\d|WINXP|WIN2000|WIN98|WIN95|NT4SP5|RUNASADMIN|RUNASHIGHEST)$'
$changes = @()
foreach ($c in Get-CompatValues) {
    if ($c.Name -notlike '*\RT2_PLAT.exe') { continue }
    $tokens = @($c.Value -split '\s+' | Where-Object { $_ })
    $kept = @($tokens | Where-Object { $_ -notmatch $xpLayers })
    if ($kept.Count -eq $tokens.Count) { Write-Ok "already fine: $($c.Key)"; continue }
    $new = $kept -join ' '
    if ($new.Trim() -in @('', '$', '~')) { $new = $null }
    $changes += [pscustomobject]@{ Key = $c.Key; Name = $c.Name; Value = $new }
}
if ($changes.Count -and -not (Set-CompatValues $changes)) {
    Write-Warn2 'The game will still ask for admin rights on launch.'
}
