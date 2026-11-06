<#
.SYNOPSIS
  Run the GUI test scripts of tools/gui_tests on the invisible desktop (tools/run_gui_test.ps1).

.DESCRIPTION
  Each script names its design in a line "# design: <name>":
    dff_pnr       prjpeppercorn-test-cases/035-dff-ffobf/dff-ffobf_olimex_pnr.json (placed and routed; all
                  cells FIXED by the packer)
    dff_synth     035-dff-ffobf/dff-ffobf.json with -o ccf=olimex.ccf (not packed; Pack in the GUI)
    counter_pnr   008-counter synthesized with yosys and placed and routed without the GUI (placer cells
                  WEAK, IO cells FIXED); built once into <OutDir>/designs
    counter_synth the synthesized 008-counter with its CCF (not packed; Pack in the GUI)
  Results: <OutDir>/<script>/ (guitest.log, PNGs). Prints one line per script and returns the number of
  failed scripts.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\gui_tests\run_all.ps1 -OutDir C:\tmp\gt
  powershell ... -File tools\gui_tests\run_all.ps1 -OutDir C:\tmp\gt -Only placement_2a
#>
param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$Cases = "C:\Users\mio\source\prjpeppercorn-test-cases",
    [string]$Yosys = "",
    [string]$Only = "",
    [int]$TimeoutSec = 240
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = Split-Path -Parent (Split-Path -Parent $here)
$exe = Join-Path $root "build\Release\nextpnr-himbaechel.exe"
if ($Yosys -eq "") {
    $Yosys = if (Test-Path "C:\oss-cad-suite\bin\yosys.exe") { "C:\oss-cad-suite\bin\yosys.exe" } else { "yosys" }
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$designs = Join-Path $OutDir "designs"
New-Item -ItemType Directory -Force $designs | Out-Null

function Get-Design([string]$name) {
    $counterCcf = "-o ccf=$Cases\008-counter\olimex.ccf"
    switch ($name) {
        "dff_pnr" { return @{ Json = "$Cases\035-dff-ffobf\dff-ffobf_olimex_pnr.json"; Extra = "" } }
        "dff_synth" {
            return @{ Json = "$Cases\035-dff-ffobf\dff-ffobf.json"; Extra = "-o ccf=$Cases\035-dff-ffobf\olimex.ccf" }
        }
        { $_ -in "counter_pnr", "counter_synth" } {
            $synth = Join-Path $designs "counter.json"
            $pnr = Join-Path $designs "counter_pnr.json"
            if (-not (Test-Path $synth)) {
                Push-Location $designs
                & $Yosys -ql synth.log -p "read_verilog -sv $Cases\008-counter\top.v; synth_gatemate -luttree -top top -json counter.json"
                Pop-Location
                if (-not (Test-Path $synth)) { throw "could not synthesize $synth (see $designs\synth.log)" }
            }
            if ($name -eq "counter_synth") { return @{ Json = $synth; Extra = $counterCcf } }
            if (-not (Test-Path $pnr)) {
                # without --gui no window opens; run it on the hidden desktop anyway (no console either)
                & (Join-Path $root "tools\hidden_desktop.ps1") -Exe $exe -WorkDir $designs `
                    -Arguments "--device CCGM1A1 --json `"$synth`" $counterCcf --write `"$pnr`"" `
                    -Stdout (Join-Path $designs "pnr.log") -Stderr (Join-Path $designs "pnr_err.log") -TimeoutSec 600 | Out-Null
                if (-not (Test-Path $pnr)) { throw "could not build $pnr (see $designs\pnr.log)" }
            }
            return @{ Json = $pnr; Extra = "" }
        }
        default { throw "unknown design '$name'" }
    }
}

$failed = 0
$scripts = Get-ChildItem $here -Filter *.txt | Sort-Object Name
foreach ($s in $scripts) {
    $base = [IO.Path]::GetFileNameWithoutExtension($s.Name)
    if ($Only -ne "" -and $base -notlike "*$Only*") { continue }
    $line = Select-String -Path $s.FullName -Pattern "^#\s*design:\s*(\S+)" | Select-Object -First 1
    $dname = if ($line) { $line.Matches[0].Groups[1].Value } else { "dff_pnr" }
    $d = Get-Design $dname
    $out = Join-Path $OutDir $base
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    $start = Get-Date
    & (Join-Path $root "tools\run_gui_test.ps1") -Json $d.Json -ExtraArgs $d.Extra -Script $s.FullName `
        -OutDir $out -TimeoutSec $TimeoutSec | Out-Null
    $code = $LASTEXITCODE
    $secs = [int]((Get-Date) - $start).TotalSeconds
    $result = if ($code -eq 0) { "PASS" } else { "FAIL ($code)"; $failed++ }
    $err = ""
    if ($code -ne 0 -and (Test-Path "$out\guitest.log")) {
        $err = (Select-String -Path "$out\guitest.log" -Pattern "ERROR|CRASH" | Select-Object -First 1).Line
    }
    Write-Output ("{0,-28} {1,-10} {2,4}s {3}" -f $base, $result, $secs, $err)
}
Write-Output "results: $OutDir"
exit $failed
