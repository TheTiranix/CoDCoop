# walk.ps1 host|join [segundos] [tecla]: mantiene una tecla (W por defecto) en la ventana de ese juego, sin
# traerla al frente (WM_KEYDOWN/WM_KEYUP con PostMessage). Para probar movimiento sin teclado.
param([string]$Who = 'join', [double]$Seconds = 2, [string]$Key = 'W')
Add-Type 'using System; using System.Runtime.InteropServices; public static class K { [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l); [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint c, uint t); }'
$ps = @(Get-Process CoDSP,CoDUOSP -ErrorAction SilentlyContinue | Where-Object { $_.Path -like 'D:\CoDCoop\*' } | Sort-Object StartTime)
$p = if ($Who -eq 'host') { $ps[0] } else { $ps[-1] }
$vk = if ($Key -eq 'ESC') { [uint32]0x1B } else { [uint32][char]$Key.ToUpper() }
$scan = [K]::MapVirtualKey($vk, 0)
$down = [IntPtr](1 -bor ($scan -shl 16))
$up = [IntPtr]([int64]1 -bor ($scan -shl 16) -bor (1 -shl 30) -bor (1 -shl 31))
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) { [K]::PostMessage($p.MainWindowHandle, 0x100, [IntPtr]$vk, $down) | Out-Null; Start-Sleep -Milliseconds 50 }
[K]::PostMessage($p.MainWindowHandle, 0x101, [IntPtr]$vk, $up) | Out-Null
