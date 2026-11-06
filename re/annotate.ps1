# Writes rt2_plat.annotated.asm: the disassembly with IAT slots replaced by "DLL!Function".
$exe = Join-Path (Split-Path $PSScriptRoot -Parent) 'backup\original\files\RT2_PLAT.EXE'
$b = [IO.File]::ReadAllBytes($exe)
$pe = [BitConverter]::ToInt32($b, 0x3C)
$optSize = [BitConverter]::ToUInt16($b, $pe + 20)
$nsec = [BitConverter]::ToUInt16($b, $pe + 6)
$imageBase = [BitConverter]::ToUInt32($b, $pe + 52)
$secs = @()
for ($i = 0; $i -lt $nsec; $i++) {
    $o = $pe + 24 + $optSize + $i * 40
    $secs += [pscustomobject]@{ VA = [BitConverter]::ToUInt32($b, $o + 12); VS = [BitConverter]::ToUInt32($b, $o + 8); Raw = [BitConverter]::ToUInt32($b, $o + 20) }
}
function R2O($rva) { foreach ($s in $secs) { if ($rva -ge $s.VA -and $rva -lt $s.VA + [Math]::Max($s.VS, 1)) { return $rva - $s.VA + $s.Raw } }; -1 }
function CStr($off) { $e = $off; while ($b[$e] -ne 0) { $e++ }; [Text.Encoding]::ASCII.GetString($b, $off, $e - $off) }

$map = @{}
$o = R2O ([BitConverter]::ToUInt32($b, $pe + 24 + 104))
while ($true) {
    $oft = [BitConverter]::ToUInt32($b, $o); $nameRva = [BitConverter]::ToUInt32($b, $o + 12); $ft = [BitConverter]::ToUInt32($b, $o + 16)
    if ($nameRva -eq 0) { break }
    $dll = (CStr (R2O $nameRva)) -replace '\.dll$', ''
    $th = R2O $(if ($oft) { $oft } else { $ft }); $slot = $imageBase + $ft
    while ($true) {
        $v = [BitConverter]::ToUInt32($b, $th); if ($v -eq 0) { break }
        $fn = if ($v -band 0x80000000) { '#' + ($v -band 0xFFFF) } else { CStr ((R2O $v) + 2) }
        $map[('0x{0:x}' -f $slot)] = "$dll!$fn"
        $th += 4; $slot += 4
    }
    $o += 20
}
$map.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name) $($_.Value)" } | Set-Content (Join-Path $PSScriptRoot 'imports.txt')

$src = Join-Path $PSScriptRoot 'rt2_plat.asm'
$dst = Join-Path $PSScriptRoot 'rt2_plat.annotated.asm'
$rx = [regex]'\[(0x58[0-9a-f]{4})\]'
$reader = [IO.StreamReader]::new($src); $writer = [IO.StreamWriter]::new($dst)
while (($line = $reader.ReadLine()) -ne $null) {
    $m = $rx.Match($line)
    if ($m.Success -and $map.ContainsKey($m.Groups[1].Value)) { $line = $line + '    ; ' + $map[$m.Groups[1].Value] }
    $writer.WriteLine($line)
}
$reader.Close(); $writer.Close()
"$($map.Count) imports mapped"
