# Uso: .\gh.ps1 <programa> Decompile <salida> <hex> [<hex>...]   (programa: CoDSP.exe | gamex86.dll | cgamex86.dll)
#      .\gh.ps1 <programa> XrefsTo <salida> <hex>...   |   .\gh.ps1 <programa> ListInstr <salida> <ini> <fin>
param([string]$Prog, [string]$Script, [string]$Out, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)
$gh = 'D:\Tools\ghidra\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat'
$null = & $gh D:\CoDCoop\ghidra CoD -process $Prog -noanalysis -readOnly -scriptPath D:\CoDCoop\ghidra\scripts -postScript "$Script.java" $Out @Rest 2>&1
if (Test-Path $Out) { "ok: $Out ($((Get-Item $Out).Length) bytes)" } else { "FALLO" }
