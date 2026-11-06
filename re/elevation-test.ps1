# Launches the game with each given compatibility-layer string and reports whether Windows
# ran it elevated. Relies on [debug] autoquit_ms in rt2wide.ini to close the game.
param([string[]] $Layers)
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class Elev {
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint a, bool i, int pid);
  [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr p, uint a, out IntPtr t);
  [DllImport("advapi32.dll", SetLastError=true)] static extern bool GetTokenInformation(IntPtr t, int c, out int v, int l, out int r);
  public static string Check(int pid) {
    IntPtr p = OpenProcess(0x1000, false, pid); if (p == IntPtr.Zero) return "OpenProcess failed " + Marshal.GetLastWin32Error();
    IntPtr t; if (!OpenProcessToken(p, 8, out t)) return "OpenProcessToken failed " + Marshal.GetLastWin32Error() + " (likely elevated)";
    int v, r; GetTokenInformation(t, 20, out v, 4, out r); return v != 0 ? "ELEVATED" : "not elevated";
  }
}
'@
. "$PSScriptRoot\..\scripts\common.ps1"
foreach ($layer in $Layers) {
    while (Get-Process RT2_PLAT -ErrorAction SilentlyContinue) { Start-Sleep 1 }
    if ($layer -eq 'registry') { Remove-Item Env:__COMPAT_LAYER -ErrorAction SilentlyContinue } else { $env:__COMPAT_LAYER = $layer }
    Start-Process -FilePath (Join-Path $GameDir 'RT2_PLAT.EXE') -WorkingDirectory $GameDir
    Remove-Item Env:__COMPAT_LAYER -ErrorAction SilentlyContinue
    Start-Sleep 2
    $p = Get-Process RT2_PLAT -ErrorAction SilentlyContinue
    '{0} -> {1}' -f $layer, $(if ($p) { [Elev]::Check($p.Id) } else { 'not running' })
}
while (Get-Process RT2_PLAT -ErrorAction SilentlyContinue) { Start-Sleep 1 }
