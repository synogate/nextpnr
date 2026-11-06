<#
.SYNOPSIS
  Check the result of tools/gui_tests/flow_5a.txt without the GUI.

.DESCRIPTION
  In <Dir> (the test's output directory, e.g. <run_all OutDir>\flow_5a):
    - flow_5a.json: check_routed.py (every net one routing tree), the hand-placed cells on their tiles and the
      locked routing unchanged and still locked (flow_5a_hand.json, written by the test)
    - bitstream.txt: packed with gmpack
    - post-route netlist (yosys read_json / write_verilog with the test case's cpesim.v) proven equivalent to the
      008-counter source with eqy
  Returns the number of failed checks.
#>
param(
    [Parameter(Mandatory = $true)][string]$Dir,
    [string]$Cases = "C:\Users\mio\source\prjpeppercorn-test-cases",
    [string]$OssCad = "C:\oss-cad-suite"
)

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = Join-Path $root "build\Release\nextpnr-himbaechel.exe"
$Dir = (Resolve-Path $Dir).Path
# (eqy's make runs bash scripts: Git's bash, not WSL's from system32)
$gitBash = "C:\Program Files\Git\usr\bin"
$env:PATH = "$OssCad\bin;$OssCad\lib;$gitBash;$env:PATH"
$failed = 0
Push-Location $Dir
try {
    # routing trees, hand-placed cells and locked routing in the saved design
    $hand = Join-Path $Dir "check_hand.py"
    @'
import json
h = json.load(open("flow_5a_hand.json"))
bad = []
cells = {n: c for n, c in ctx.cells}
for n, (tile, strength) in h["cells"].items():
    if n in cells and (str(cells[n].bel).split("/")[0], int(cells[n].belStrength)) != (tile, strength):
        bad.append("cell %s at %s" % (n, cells[n].bel))
for net, R in h["locked_routing"].items():
    T = {str(w): (str(m.pip), int(m.strength)) for w, m in ctx.nets[net].wires}
    for w, p in R.items():
        if T.get(w) != (p, 6):
            bad.append("net %s wire %s: %s" % (net, w, T.get(w)))
print("HAND bad: %d %s" % (len(bad), bad[:5]))
'@ | Set-Content -Encoding utf8 $hand
    $out = & $exe --device CCGM1A1 --json flow_5a.json --run (Join-Path $PSScriptRoot "check_routed.py") --run $hand 2>&1
    $out | Select-String "CHECK|HAND|BAD" | ForEach-Object { Write-Output $_.Line }
    if (-not ($out | Select-String "bad: 0,|bad: 0$")) { $failed++ }
    if (-not ($out | Select-String "HAND bad: 0")) { $failed++ }

    & gmpack bitstream.txt bitstream.bit
    if ($LASTEXITCODE -ne 0) { Write-Output "gmpack FAILED"; $failed++ } else { Write-Output "gmpack ok" }

    & yosys -q -p "read_json flow_5a.json; write_verilog -norename -noattr flow_5a_pnr.v"
    $c = $Cases -replace '\\', '/'
    @"
[gold]
read_verilog -nowb -noblackbox +/gatemate/cells_sim.v $c/cells_extra.v
read_verilog -defer -sv $c/olimex.vh $c/008-counter/top.v
prep -top top
flatten
async2sync
memory_map

[gate]
read_verilog -nowb -noblackbox +/gatemate/cells_sim.v $c/cpesim.v
read_verilog flow_5a_pnr.v
prep -top top
flatten
async2sync
memory_map

[collect *]
bind *

[strategy pdr]
use sby
engine abc scorr; pdr
"@ | Set-Content -Encoding ascii check_pnr.eqy
    # (eqy's summary step fails on Windows; the partition results are in its log)
    $eqy = & eqy -f check_pnr.eqy 2>&1
    $proved = $eqy | Select-String "Proved equivalence"
    $notproved = $eqy | Select-String "Failed to prove|failed to prove|Could not prove"
    $proved | ForEach-Object { Write-Output $_.Line }
    if (-not $proved -or $notproved) { Write-Output "eqy FAILED"; $notproved | ForEach-Object { Write-Output $_.Line }; $failed++ }
} finally {
    Pop-Location
}
Write-Output "failed checks: $failed"
exit $failed
