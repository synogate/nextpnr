<#
.SYNOPSIS
  Run a scripted nextpnr GUI test on an invisible desktop (nothing appears on the screen).

.DESCRIPTION
  Starts nextpnr-himbaechel --gui on a hidden desktop (tools/hidden_desktop.ps1) with
  NEXTPNR_GUI_TEST=<script> (commands: see gui/guitest.h). Relative file names in `shot` commands
  are relative to -OutDir. Writes <OutDir>/guitest.log (driver steps + Qt messages),
  <OutDir>/stdout.txt and <OutDir>/stderr.txt, prints the log, the PNGs written and returns the
  exit code (0 passed, 1 failed step, 124 timeout).

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\run_gui_test.ps1 `
     -Json C:\path\design.json -Script test.txt -OutDir out
#>
param(
    [string]$Json = "",
    [Parameter(Mandatory = $true)][string]$Script,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$Device = "CCGM1A1",
    # further nextpnr arguments, e.g. "-o ccf=board.ccf"
    [string]$ExtraArgs = "",
    [string]$Exe = "",
    [int]$TimeoutSec = 180,
    # use Mesa's software OpenGL (opengl32sw.dll must be next to the exe)
    [switch]$SoftwareGL
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ($Exe -eq "") { $Exe = Join-Path $root "build\Release\nextpnr-himbaechel.exe" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$Script = (Resolve-Path $Script).Path

$log = Join-Path $OutDir "guitest.log"
Remove-Item -ErrorAction SilentlyContinue $log
$before = Get-Date

$env:NEXTPNR_GUI_TEST = $Script
$env:NEXTPNR_GUI_TEST_LOG = $log
if ($SoftwareGL) { $env:QT_OPENGL = "software" }

$nargs = "--device $Device --gui"
if ($Json -ne "") { $nargs += " --json `"$((Resolve-Path $Json).Path)`"" }
if ($ExtraArgs -ne "") { $nargs += " $ExtraArgs" }

& (Join-Path $PSScriptRoot "hidden_desktop.ps1") -Exe $Exe -Arguments $nargs -WorkDir $OutDir `
    -Stdout (Join-Path $OutDir "stdout.txt") -Stderr (Join-Path $OutDir "stderr.txt") -TimeoutSec $TimeoutSec
$code = $LASTEXITCODE

Remove-Item Env:NEXTPNR_GUI_TEST, Env:NEXTPNR_GUI_TEST_LOG -ErrorAction SilentlyContinue
if (Test-Path $log) { Get-Content $log } else { Write-Output "(no guitest.log - see $OutDir\stderr.txt)" }
Write-Output "---"
Write-Output "log: $log"
Get-ChildItem $OutDir -Recurse -Filter *.png | Where-Object { $_.LastWriteTime -ge $before } |
    ForEach-Object { Write-Output "png: $($_.FullName)" }
Write-Output "exit code: $code"
exit $code
