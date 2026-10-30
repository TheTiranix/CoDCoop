# Lee memoria de un proceso (sirve aunque este colgado).  .\peek.ps1 <pid> <dir hex> <bytes>  -> dwords en hex
param([int]$ProcId, [string]$Addr, [int]$Len = 64)
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class P2 {
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(int a, bool i, int id);
  [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr p, IntPtr a, byte[] b, int n, out IntPtr r);
}
'@
$h = [P2]::OpenProcess(0x10, $false, $ProcId)
$b = New-Object byte[] $Len; $r = [IntPtr]::Zero
[P2]::ReadProcessMemory($h, [IntPtr][int64][Convert]::ToUInt32($Addr, 16), $b, $Len, [ref]$r) | Out-Null
$out = @()
for ($i = 0; $i -lt $Len; $i += 4) { $out += ('{0:x}:{1:x8}' -f $i, [BitConverter]::ToUInt32($b, $i)) }
$out -join ' '
