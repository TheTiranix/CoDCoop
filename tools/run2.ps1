# Pruebas de CoD2 con 2 juegos en la copia de desarrollo (game2\).
#   .\run2.ps1 host [mapa]       abre el host (devmap + coop2host)
#   .\run2.ps1 join [mapa]       abre el que se une (perfil aparte en run\join2, devmap + coop2join 127.0.0.1)
#   .\run2.ps1 join3|join4 [mapa]  3er/4to jugador (perfil run\join3/run\join4, registro cod2coop_join3.log...)
#   .\run2.ps1 click             clic en todas las ventanas de prueba ("haz clic para empezar")
#   .\run2.ps1 key <seg> <tecla> [host|join]   mantiene una tecla
#   .\run2.ps1 kill              cierra solo los CoD2 de D:\CoDCoop
param([string]$What, [string]$A1 = 'moscow', [string]$A2, [string]$A3 = 'join')
$game = 'D:\CoDCoop\game2'
Add-Type 'using System; using System.Runtime.InteropServices; public static class R2 { [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l); [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint c, uint t); }'
function Games { @(Get-Process CoD2SP_s -ErrorAction SilentlyContinue | Where-Object { $_.Path -like 'D:\CoDCoop\*' } | Sort-Object StartTime) }
# host = el primero abierto; join = el ultimo (o el 2do si hay mas de dos); join3/join4 = el 3ro/4to
function Pick($who) { $ps = Games; switch ($who) { 'host' { $ps[0] } 'join3' { $ps[2] } 'join4' { $ps[3] } default { if ($ps.Count -gt 2) { $ps[1] } else { $ps[-1] } } } }
switch ($What) {
  'host' {
    Remove-Item "$game\__CoD2SP_s" -Force -ErrorAction SilentlyContinue
    $a = "+set r_fullscreen 0 +set logfile 2 +set com_introplayed 1 +set com_maxfps 125 +devmap $A1 +wait 300 +coop2host"
    (Start-Process "$game\CoD2SP_s.exe" -WorkingDirectory $game -ArgumentList $a -PassThru).Id
  }
  { $_ -in 'join', 'join3', 'join4' } {
    Remove-Item "$game\__CoD2SP_s" -Force -ErrorAction SilentlyContinue
    $prof = if ($What -eq 'join') { 'join2' } else { $What }
    if (!(Test-Path "D:\CoDCoop\run\$prof\main\players")) {
      New-Item -ItemType Directory -Force "D:\CoDCoop\run\$prof\main" | Out-Null
      Copy-Item -Recurse 'D:\CoDCoop\run\join2\main\players' "D:\CoDCoop\run\$prof\main\"
    }
    $a = "+set fs_homepath D:\CoDCoop\run\$prof +set r_fullscreen 0 +set logfile 2 +set com_introplayed 1 +set com_maxfps 125 +devmap $A1 +wait 300 +coop2join 127.0.0.1"
    (Start-Process "$game\CoD2SP_s.exe" -WorkingDirectory $game -ArgumentList $a -PassThru).Id
  }
  'joinmenu' {  # el que se une queda en el menu principal y pide entrar (carga solo el mapa del host)
    Remove-Item "$game\__CoD2SP_s" -Force -ErrorAction SilentlyContinue
    $a = "+set fs_homepath D:\CoDCoop\run\join2 +set r_fullscreen 0 +set logfile 2 +set com_introplayed 1 +set com_maxfps 125 +wait 300 +coop2join 127.0.0.1"
    (Start-Process "$game\CoD2SP_s.exe" -WorkingDirectory $game -ArgumentList $a -PassThru).Id
  }
  'click' {
    $l = [IntPtr](200 -bor (200 -shl 16))
    foreach ($p in Games) {
      [R2]::PostMessage($p.MainWindowHandle, 0x201, [IntPtr]1, $l) | Out-Null; Start-Sleep -Milliseconds 80
      [R2]::PostMessage($p.MainWindowHandle, 0x202, [IntPtr]0, $l) | Out-Null
    }
  }
  'key' {
    $p = Pick $A3
    $named = @{ LEFT = 0x25; UP = 0x26; RIGHT = 0x27; DOWN = 0x28; SPACE = 0x20; ESC = 0x1B; ENTER = 0x0D; TAB = 0x09 }
    $vk = if ($named.ContainsKey($A2.ToUpper())) { $named[$A2.ToUpper()] } else { [int][char]$A2.ToUpper() }
    $sc = [R2]::MapVirtualKey($vk, 0); $ext = if ($vk -ge 0x25 -and $vk -le 0x28) { 0x1000000 } else { 0 }
    $l = [IntPtr](1 -bor ($sc -shl 16) -bor $ext)
    $end = (Get-Date).AddSeconds([double]$A1)
    while ((Get-Date) -lt $end) { [R2]::PostMessage($p.MainWindowHandle, 0x100, [IntPtr]$vk, $l) | Out-Null; Start-Sleep -Milliseconds 50 }
    [R2]::PostMessage($p.MainWindowHandle, 0x101, [IntPtr]$vk, [IntPtr](1 -bor ($sc -shl 16) -bor 0xC0000000)) | Out-Null
  }
  'cmd' {  # cmd host|join|join3|join4 "<comando de consola>"
    $f = if ($A1 -like 'join*') { "$game\cod2coop_$A1.cmd" } else { "$game\cod2coop.cmd" }
    for ($i = 0; $i -lt 30 -and (Test-Path $f); $i++) { Start-Sleep -Milliseconds 100 }
    Set-Content -Path $f -Value $A2 -Encoding ascii -NoNewline
  }
  'shot' {  # shot <archivo.png> [host|join]: captura esa ventana (la pone adelante un momento)
    Add-Type 'using System; using System.Runtime.InteropServices; public static class J2 { [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f); [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r); [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); public struct R { public int L, T, Rt, B; } }'
    Add-Type -AssemblyName System.Drawing
    [J2]::SetProcessDPIAware() | Out-Null
    $p = Pick $(if ($A2) { $A2 } else { 'host' })
    [J2]::SetWindowPos($p.MainWindowHandle, [IntPtr](-1), 0, 0, 0, 0, 0x13) | Out-Null; Start-Sleep -Milliseconds 400
    $r = New-Object J2+R; [J2]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.Rt - $r.L), ($r.B - $r.T)
    [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $bmp.Save($A1)
    [J2]::SetWindowPos($p.MainWindowHandle, [IntPtr](-2), 0, 0, 0, 0, 0x13) | Out-Null
    $A1
  }
  'kill' {
    Games | Stop-Process -Force
    Start-Sleep 1
    Remove-Item "$game\__CoD2SP_s" -Force -ErrorAction SilentlyContinue
  }
}
