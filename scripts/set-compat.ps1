# Applies Windows compatibility-layer values listed in a JSON file: [{Key, Name, Value}], where a
# null Value deletes the entry. Run elevated when HKLM entries are involved. Only entries for
# the game's own folder under the known AppCompatFlags keys are accepted.
param([Parameter(Mandatory)] [string] $Json)
. "$PSScriptRoot\common.ps1"

$items = @(Get-Content $Json -Raw | ConvertFrom-Json | ForEach-Object { $_ })
foreach ($i in $items) {
    if ($CompatKeys -notcontains $i.Key -or $i.Name -notlike "$GameDir\*") {
        throw "Refusing to change unexpected compatibility entry: $($i.Key) / $($i.Name)"
    }
}
foreach ($i in $items) {
    if ($null -eq $i.Value) {
        Remove-ItemProperty -Path $i.Key -Name $i.Name -ErrorAction SilentlyContinue
        Write-Ok "removed $($i.Key)"
    } else {
        if (-not (Test-Path $i.Key)) { New-Item -Path $i.Key -Force | Out-Null }
        Set-ItemProperty -Path $i.Key -Name $i.Name -Value $i.Value
        Write-Ok "set $($i.Key) = '$($i.Value)'"
    }
}
