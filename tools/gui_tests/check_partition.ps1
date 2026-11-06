<#
.SYNOPSIS
  Check the partition saved by tools/gui_tests/partition_5b.txt without the GUI: import it.

.DESCRIPTION
  In <Dir> (the test's output directory, e.g. <run_all OutDir>\partition_5b): synthesizes a top level that
  instantiates the counter partition (module core) as the blackbox u_core, imports core_part.json with
  --vopt partition=u_core:core_part.json (placement and the hand routing taken over), places and routes the
  rest and writes the bitstream; then checks the routed result with check_routed.py, that the partition's
  cells kept their bels, and packs the bitstream with gmpack. Returns the number of failed checks.
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
$env:PATH = "$OssCad\bin;$OssCad\lib;$env:PATH"
$failed = 0
Push-Location $Dir
try {
    (Get-Content "$Cases\008-counter\top.v") -replace '^module top', 'module core' | Set-Content core.v
    @'
module top (input clk, input rst, output [7:0] led);
    core u_core (.clk(clk), .rst(rst), .led(led));
endmodule
'@ | Set-Content wrap.v
    & yosys -ql wrap_synth.log -p "read_verilog -sv wrap.v core.v; blackbox core; setattr -set clkbuf_sink 1 =core/w:clk; synth_gatemate -luttree -top top; write_json wrap.json"
    if (-not (Test-Path wrap.json)) { Write-Output "yosys FAILED"; return 1 }

    $out = & $exe --device CCGM1A1 --json wrap.json --vopt ccf="$Cases\008-counter\olimex.ccf" `
        --vopt partition=u_core:core_part.json --vopt out=import.txt --write import.json 2>&1
    $out | Set-Content import.log
    $out | Select-String "partition|ERROR" | Select-Object -First 8 | ForEach-Object { Write-Output $_.Line }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path import.json)) { Write-Output "import FAILED (see import.log)"; $failed++ }
    else {
        # the partition's cells (u_core.<cell>) on the bels they have in core_part.json
        $keep = Join-Path $Dir "check_keep.py"
        @'
import json
part = json.load(open("core_part.json"))
cells = list(part["modules"].values())[0]["cells"]
want = {"u_core." + n: c["attributes"].get("NEXTPNR_BEL") for n, c in cells.items() if "NEXTPNR_BEL" in c["attributes"]}
have = {n: str(c.bel) for n, c in ctx.cells}
bad = [(n, b, have.get(n)) for n, b in want.items() if n in have and have[n] != b]
print("KEEP partition cells: %d, found: %d, moved: %d %s" % (len(want), sum(1 for n in want if n in have), len(bad), bad[:4]))
'@ | Set-Content -Encoding utf8 $keep
        $chk = & $exe --device CCGM1A1 --json import.json --run (Join-Path $PSScriptRoot "check_routed.py") --run $keep 2>&1
        $chk | Select-String "CHECK|BAD|KEEP" | ForEach-Object { Write-Output $_.Line }
        if (-not ($chk | Select-String "bad: 0,|bad: 0$")) { $failed++ }
        if (-not ($chk | Select-String "KEEP .* moved: 0")) { $failed++ }
        & gmpack import.txt import.bit
        if ($LASTEXITCODE -ne 0) { Write-Output "gmpack FAILED"; $failed++ } else { Write-Output "gmpack ok" }
    }
} finally {
    Pop-Location
}
Write-Output "failed checks: $failed"
exit $failed
