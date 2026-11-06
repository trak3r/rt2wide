# Launches the game with a scripted input sequence, waits for it to auto-quit, and converts
# the frame dumps in re\frames to PNG.
#   .\run-test.ps1 -Script "wait 4000; click 95 326; wait 2000; dump" -Seconds 20
param(
    [Parameter(Mandatory)] [string] $Script,
    [int] $Seconds = 20,
    [int] $DumpEveryMs = 0
)
. "$PSScriptRoot\..\scripts\common.ps1"
Add-Type -AssemblyName System.Drawing

$frames = Join-Path $PSScriptRoot 'frames'
$ini = Join-Path $GameDir 'rt2wide.ini'
if (-not (Test-Path $ini)) { throw "Mod not installed: $ini" }
if (Get-Process RT2_PLAT -ErrorAction SilentlyContinue) { throw 'The game is already running.' }

$t = Get-Content $ini -Raw
$t = $t -replace '(?m)^dump_dir=.*$', "dump_dir=$frames"
$t = $t -replace '(?m)^dump_every_ms=.*$', "dump_every_ms=$DumpEveryMs"
$t = $t -replace '(?m)^autoquit_ms=.*$', "autoquit_ms=$($Seconds * 1000)"
$t = $t -replace '(?m)^script=.*$', "script=$Script"
Set-Content $ini $t -NoNewline

if (Test-Path $frames) { Remove-Item (Join-Path $frames '*') -Force }
Start-Process -FilePath (Join-Path $GameDir 'RT2_PLAT.EXE') -WorkingDirectory $GameDir
Start-Sleep -Seconds 3
while (Get-Process RT2_PLAT -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 500 }

Get-ChildItem $frames -Filter *.bmp -ErrorAction SilentlyContinue | ForEach-Object {
    $png = $_.FullName -replace '\.bmp$', '.png'
    $b = [System.Drawing.Bitmap]::FromFile($_.FullName)
    $b.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $b.Dispose()
    Remove-Item $_.FullName
    $png
}
