. "$PSScriptRoot\common.ps1"

$toolchain = Get-ChildItem (Join-Path $Root 'tools') -Directory -Filter 'llvm-mingw-*' | Select-Object -First 1
if (-not $toolchain) { throw 'Compiler not found under tools\. Download llvm-mingw (msvcrt, x86_64 host) and unzip it there.' }
$cxx = Join-Path $toolchain.FullName 'bin\i686-w64-mingw32-clang++.exe'

New-Item -ItemType Directory -Force $BuildDir | Out-Null
$src = Join-Path $Root 'src'
$sources = Get-ChildItem $src -Filter *.cpp | ForEach-Object { $_.FullName }
$out = Join-Path $BuildDir 'ddraw.dll'

Write-Step "Building $out"
& $cxx -std=c++17 -O2 -Wall -Wno-unused-function -Wno-missing-exception-spec -fno-exceptions -fno-rtti -shared -static `
    -o $out @sources (Join-Path $src 'ddraw.def') `
    -luser32 -lgdi32 -lopengl32 -ldxguid
if ($LASTEXITCODE -ne 0) { throw "Build failed." }
Write-Ok "Built $out"
