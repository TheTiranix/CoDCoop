# Pruebas con 2 juegos en la copia de desarrollo.
#   .\run.ps1 host [mapa]        abre el host (devmap + coophost)
#   .\run.ps1 join [mapa]        abre el que se une (devmap + coopjoin 127.0.0.1)
#   .\run.ps1 cmd host|join "<comando de consola>"
#   .\run.ps1 shot <salida.png>  captura de pantalla completa
#   .\run.ps1 kill               cierra solo los juegos de D:\CoDCoop
param([string]$What, [string]$A1 = 'pathfinder', [string]$A2)
$game = 'D:\CoDCoop\game'
switch ($What) {
  'host' {
    Remove-Item "$game\__CoDSP" -Force -ErrorAction SilentlyContinue
    Get-ChildItem $game -Filter 'codcoop*.log' | Remove-Item -Force -ErrorAction SilentlyContinue
    $args = "+set r_fullscreen 0 +set r_mode 3 +set com_introplayed 1 +set logfile 2 +set sv_cheats 1 +devmap $A1 +coophost"
    (Start-Process "$game\CoDSP.exe" -WorkingDirectory $game -ArgumentList $args -PassThru).Id
  }
  'join' {
    Remove-Item "$game\__CoDSP" -Force -ErrorAction SilentlyContinue
    $args = "+set fs_homepath D:\CoDCoop\run\join +set r_fullscreen 0 +set r_mode 3 +set com_introplayed 1 +set logfile 2 +set sv_cheats 1 +devmap $A1 +coopjoin 127.0.0.1"
    (Start-Process "$game\CoDSP.exe" -WorkingDirectory $game -ArgumentList $args -PassThru).Id
  }
  'cmd' {
    $f = if ($A1 -eq 'join') { "$game\codcoop_join.cmd" } else { "$game\codcoop.cmd" }
    for ($i = 0; $i -lt 30 -and (Test-Path $f); $i++) { Start-Sleep -Milliseconds 100 }  # que lea el anterior
    Set-Content -Path $f -Value $A2 -Encoding ascii -NoNewline
    for ($i = 0; $i -lt 30 -and (Test-Path $f); $i++) { Start-Sleep -Milliseconds 50 }   # y este
  }
  'shot' {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $bmp.Save($A1)
    $A1
  }
  'shotj' {  # captura solo la ventana del que se une (la pone adelante un momento)
    Add-Type 'using System; using System.Runtime.InteropServices; public static class J { [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f); [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r); [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); public struct R { public int L, T, Rt, B; } }'
    Add-Type -AssemblyName System.Drawing
    [J]::SetProcessDPIAware() | Out-Null  # GetWindowRect y CopyFromScreen en pixeles reales
    $p = @(Get-Process CoDSP -ErrorAction SilentlyContinue | Where-Object { $_.Path -like 'D:\CoDCoop\*' } | Sort-Object StartTime)[-1]
    [J]::SetWindowPos($p.MainWindowHandle, [IntPtr](-1), 0, 0, 0, 0, 0x13) | Out-Null; Start-Sleep -Milliseconds 400
    $r = New-Object J+R; [J]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.Rt - $r.L), ($r.B - $r.T)
    [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $bmp.Save($A1)
    [J]::SetWindowPos($p.MainWindowHandle, [IntPtr](-2), 0, 0, 0, 0, 0x13) | Out-Null
    $A1
  }
  'click' {  # clic en todas las ventanas de prueba ("Haz clic para iniciar" tras cargar); click <x> <y> en coordenadas 640x480
    Add-Type 'using System; using System.Runtime.InteropServices; public static class M { [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l); }'
    $x = if ($PSBoundParameters.ContainsKey('A1')) { [int]$A1 } else { 200 }
    $y = if ($A2) { [int]$A2 } else { 200 }
    foreach ($p in Get-Process CoDSP -ErrorAction SilentlyContinue | Where-Object { $_.Path -like 'D:\CoDCoop\*' }) {
      $l = [IntPtr]($x -bor ($y -shl 16))
      [M]::PostMessage($p.MainWindowHandle, 0x201, [IntPtr]1, $l) | Out-Null; Start-Sleep -Milliseconds 80
      [M]::PostMessage($p.MainWindowHandle, 0x202, [IntPtr]0, $l) | Out-Null
    }
  }
  'layout' {  # host a la izquierda, el que se une a la derecha; con "join" solo el que se une, entero a la izquierda
    Add-Type 'using System; using System.Runtime.InteropServices; public static class P { [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f); }'
    $ps = @(Get-Process CoDSP -ErrorAction SilentlyContinue | Where-Object { $_.Path -like 'D:\CoDCoop\*' } | Sort-Object StartTime)
    if ($A1 -eq 'join' -and $ps.Count -ge 2) { $ps = @($ps[1]) }
    $x = 0
    foreach ($p in $ps) { [P]::SetWindowPos($p.MainWindowHandle, [IntPtr](-1), $x, 0, 0, 0, 0x11) | Out-Null; $x += 650 }
    Start-Sleep 1
  }
  'unlayout' {
    Add-Type 'using System; using System.Runtime.InteropServices; public static class P { [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f); }'
    foreach ($p in Get-Process CoDSP -ErrorAction SilentlyContinue | Where-Object { $_.Path -like 'D:\CoDCoop\*' }) { [P]::SetWindowPos($p.MainWindowHandle, [IntPtr](-2), 0, 0, 0, 0, 0x13) | Out-Null }
  }
  'kill' {
    Get-Process CoDSP -ErrorAction SilentlyContinue | Where-Object { $_.Path -like 'D:\CoDCoop\*' } | Stop-Process -Force
    Start-Sleep 1
    Remove-Item "$game\__CoDSP" -Force -ErrorAction SilentlyContinue
  }
}
