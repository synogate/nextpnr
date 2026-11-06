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
    # another build of nextpnr-himbaechel (default: build\Release of this checkout)
    [string]$Exe = "",
    [Parameter(Mandatory = $true)][string]$Dir,
    [string]$Cases = "C:\Users\mio\source\prjpeppercorn-test-cases",
    [string]$OssCad = "C:\oss-cad-suite"
)

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = if ($Exe -ne "") { $Exe } else { Join-Path $root "build\Release\nextpnr-himbaechel.exe" }
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
# (finishing the routing hands the wires behind a CP line or bridge to the nets of the cells it makes for them)
derived = {}
for n, ni in ctx.nets:
    if ni.driver.cell is not None and ni.driver.cell.type in ("CPE_CPLINES", "CPE_BRIDGE"):
        for w, m in ni.wires:
            derived[str(w)] = (str(m.pip), int(m.strength))
for net, R in h["locked_routing"].items():
    T = {str(w): (str(m.pip), int(m.strength)) for w, m in ctx.nets[net].wires}
    for w, p in R.items():
        if T.get(w, derived.get(w)) != (p, 6):
            bad.append("net %s wire %s: %s" % (net, w, T.get(w, derived.get(w))))
print("HAND bad: %d %s" % (len(bad), bad[:5]))
'@ | Set-Content -Encoding utf8 $hand
    $out = & $exe --device CCGM1A1 --json flow_5a.json --run (Join-Path $PSScriptRoot "check_routed.py") --run $hand 2>&1
    $out | Select-String "CHECK|HAND|BAD" | ForEach-Object { Write-Output $_.Line }
    if (-not ($out | Select-String "bad: 0,|bad: 0$")) { $failed++ }
    if (-not ($out | Select-String "HAND bad: 0")) { $failed++ }

    & gmpack bitstream.txt bitstream.bit
    if ($LASTEXITCODE -ne 0) { Write-Output "gmpack FAILED"; $failed++ } else { Write-Output "gmpack ok" }

    # The JSON has the unconnected ports of the cells too ("x"), some of which the simulation models lack: leave
    # them out for the netlist checked
    $j = Get-Content flow_5a.json -Raw | ConvertFrom-Json
    foreach ($m in $j.modules.PSObject.Properties) {
        foreach ($c in $m.Value.cells.PSObject.Properties) {
            $open = @($c.Value.connections.PSObject.Properties | Where-Object { @($_.Value | Where-Object { $_ -ne "x" }).Count -eq 0 })
            foreach ($p in $open) {
                $c.Value.connections.PSObject.Properties.Remove($p.Name)
                $c.Value.port_directions.PSObject.Properties.Remove($p.Name)
            }
        }
    }
    $j | ConvertTo-Json -Depth 64 -Compress | Set-Content -Encoding ascii flow_5a_eqy.json
    & yosys -q -p "read_json flow_5a_eqy.json; write_verilog -norename -noattr flow_5a_pnr.v"
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
