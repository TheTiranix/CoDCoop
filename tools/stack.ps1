# Muestra EIP y direcciones de retorno probables de la pila de cada hilo de un proceso de 32 bits colgado.
#   .\stack.ps1 <pid>
param([int]$ProcId)
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct CTX {
    public uint ContextFlags, Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 112)] public byte[] Float;
    public uint SegGs, SegFs, SegEs, SegDs, Edi, Esi, Ebx, Edx, Ecx, Eax, Ebp, Eip, SegCs, EFlags, Esp, SegSs;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 512)] public byte[] Ext;
  }
  [DllImport("kernel32.dll")] public static extern IntPtr OpenThread(int a, bool i, int id);
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(int a, bool i, int id);
  [DllImport("kernel32.dll")] public static extern int SuspendThread(IntPtr h);
  [DllImport("kernel32.dll")] public static extern int ResumeThread(IntPtr h);
  [DllImport("kernel32.dll")] public static extern bool Wow64GetThreadContext(IntPtr h, ref CTX c);
  [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr p, IntPtr a, byte[] b, int n, out IntPtr r);
}
'@
$p = Get-Process -Id $ProcId
$hp = [W]::OpenProcess(0x10, $false, $ProcId)
foreach ($t in $p.Threads) {
  $h = [W]::OpenThread(0x1FFFFF, $false, $t.Id)
  [W]::SuspendThread($h) | Out-Null
  $c = New-Object W+CTX; $c.ContextFlags = 0x10007; $c.Float = New-Object byte[] 112; $c.Ext = New-Object byte[] 512
  $ok = [W]::Wow64GetThreadContext($h, [ref]$c)
  $buf = New-Object byte[] 0x2000; $r = [IntPtr]::Zero
  [W]::ReadProcessMemory($hp, [IntPtr][int64]$c.Esp, $buf, $buf.Length, [ref]$r) | Out-Null
  [W]::ResumeThread($h) | Out-Null
  if (-not $ok) { continue }
  $rets = @()
  for ($i = 0; $i -lt $buf.Length; $i += 4) {
    $v = [BitConverter]::ToUInt32($buf, $i)
    if (($v -ge 0x401000 -and $v -lt 0x560000) -or ($v -ge 0x10000000 -and $v -lt 0x7f000000)) { $rets += ('{0:x}' -f $v) }
  }
  '{0}: eip={1:x} esp={2:x} eax={3:x} ecx={4:x} esi={5:x} edi={6:x} | {7}' -f $t.Id, $c.Eip, $c.Esp, $c.Eax, $c.Ecx, $c.Esi, $c.Edi, (($rets | Select-Object -First 24) -join ' ')
}
