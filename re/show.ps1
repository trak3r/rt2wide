# Print disassembly around an address: .\show.ps1 4fbd59 [before] [after]
param([string]$Addr, [int]$Before = 40, [int]$After = 40)
$asm = Join-Path $PSScriptRoot 'rt2_plat.annotated.asm'
$a = $Addr.ToLower() -replace '^0x', ''
Select-String -Path $asm -Pattern "^\s+${a}:" -Context $Before, $After | ForEach-Object {
    $_.Context.PreContext; ">>" + $_.Line; $_.Context.PostContext
}
