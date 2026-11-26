# Timing constraints (SDC)

The timing constraints come from an SDC file (`--sdc top.sdc`). It is read before packing, so object names
are those of the synthesised netlist (as in the JSON file).

## Commands

| Command | Notes |
|---|---|
| `create_clock -period <ns> [-name <name>] <objects>` | on nets, cell pins or ports. The name (else the net's name) stays with the constraint when the packer copies it behind a clock buffer, and `get_clocks` finds the clock by it. |
| `create_generated_clock [-name <name>] [-source <object>] [-invert] <output port>` | a clock the design puts out at a port, as the reference of input and output delays, see below. Not for clocks inside the design (dividers, PLL outputs). |
| `set_multicycle_path <n> [-setup\|-hold] [-start\|-end] [-from <objects>] [-to <objects>]` | see below |
| `set_false_path [-setup\|-hold] [-from <objects>] [-to <objects>]` | the paths are not timed, see below |
| `set_input_delay -clock <clock> [-clock_fall] [-max\|-min] [-add_delay] <ns> <ports>` | what is outside of the device before an input, see below |
| `set_output_delay -clock <clock> [-clock_fall] [-max\|-min] [-add_delay] <ns> <ports>` | what is outside of the device after an output, see below |
| `get_cells`, `get_pins`, `get_nets`, `get_ports`, `get_clocks` | names or patterns with `*` and `?` (brackets are bus indices, `{a[3]}`) |
| `all_inputs`, `all_outputs` | the top level ports of the direction (bidirectional ones in both) |

## Multicycle paths

`set_multicycle_path` follows the SDC semantics:

- `-setup n` (the default): the path has `n` clock periods. The capture edge moves `n - 1` periods later (with
  `-end`, the default for setup, periods of the capture clock; with `-start`, of the launch clock).
- The hold check stays one period before the setup check, so it moves along with it: a setup multicycle of
  `n` alone requires the data to stay stable for `n - 1` periods after the launch edge.
- `-hold m` moves the hold check `m` periods back (`-start`, the launch clock, by default). The usual pair is

  ```tcl
  set_multicycle_path 2 -setup -from [get_cells div_reg*] -to [get_cells result_reg*]
  set_multicycle_path 1 -hold  -from [get_cells div_reg*] -to [get_cells result_reg*]
  ```

  which allows two cycles and keeps the hold check at the launch edge.

`-from` and `-to` take:

- cells: every start (or end) point of the cell;
- pins: the start or end point itself, or a clock pin (every point clocked by it). The packer renames register
  pins (`D` becomes `DIN`): a pin that no longer exists stands for the whole cell;
- nets and ports: the start point driving the net, or the end points it reaches;
- clocks: every start or end point of the clock.

A list given, whose objects all match nothing, drops the constraint (with a warning) instead of applying it to
every path. Without `-from` and `-to` a constraint applies to every path (also warned).

Of several constraints for one path, the most specific wins (`-from` and `-to`, then `-from`, then `-to`), of
equally specific ones the last. Setup and hold constraints are chosen separately. Paths from or to I/O pads
without an input or output delay (asynchronous start and end points) are not timed against a clock, so
multicycle constraints on them have no effect. `get_ports` names the pad of a port: the paths from an input, or
to an output.

## False paths

`set_false_path` takes paths out of the timing analysis: both checks, or with `-setup` or `-hold` that check
only. `-from` and `-to` are as for multicycle paths (`-through` is not supported). A false path goes before
every multicycle path on the same paths, however specific that is. If it is false for setup only, the hold check
stays where the multicycle paths put it.

A path that is false for setup:

- has no setup slack, takes no part in Fmax, the critical path reports, the slack histogram and TNS;
- has criticality 0 and no slack for the placers, the router and the repair passes: they do not work on it, and
  it may get slower than the clock allows;
- gives the hold fix no setup limit (where a hold check is left).

A path that is false for hold has no hold check: no hold violation in any corner, and the hold fix leaves it
alone. Paths from or to I/O pads leave the cross-domain reports.

The timing report says how much is taken out (`False paths: N end point(s) without a setup check, M without a
hold check`, end points counted per launch clock edge). Check the numbers: a pattern that matches more than
intended hides real violations.

## Input and output delays

Without them, the paths from an input pad and to an output pad are unclocked: they are reported (`Max delay
<async> -> posedge clk`), but have no requirement. `set_input_delay` and `set_output_delay` say what is
outside of the device on these paths, against a clock of the design, and make them paths of that clock:

```tcl
create_clock -name sys -period 10 [get_ports clk]
# the device driving din: clock-to-output 1.0 to 3.5 ns, the traces 0.5 ns (data) and 0.5 ns (clock to it)
set_input_delay -clock sys -max 4.0 [get_ports {din[*]}]
set_input_delay -clock sys -min 1.5 [get_ports {din[*]}]
# the device dout goes to: setup 2.0 ns, hold 0.5 ns, the trace 0.5 ns
set_output_delay -clock sys -max  2.5 [get_ports {dout[*]}]
set_output_delay -clock sys -min  0.0 [get_ports {dout[*]}]
```

- **Input delay**: the data is at the port from `-min` to `-max` after the clock edge. Maximum: the greatest
  clock-to-output time of the device driving the port, plus the data trace, plus the clock trace to that device,
  less the clock trace to the FPGA. Minimum: the same with the least values.
- **Output delay**: the data must be at the port `-max` before the clock edge, and must not change until `-min`
  before it. Maximum: the setup time of the device the port goes to, plus the data trace, less the difference of
  the clock traces (to that device less to the FPGA). Minimum: the data trace less the hold time of that device
  (and that difference); it is often negative, which is a time after the edge that the data has to outlast.
- The reference is the clock edge at the source of the clock: its pad. (For a device that is clocked by a clock
  the design puts out, it is the edge at that port: see "Against a clock the design puts out" below.)
  `-clock_fall` takes the falling edge.
- A delay without `-max` and `-min` is both. With `-max` alone the paths have a setup check and no hold check,
  with `-min` alone the reverse. Delays may be negative.
- A later command for a port and clock edge replaces the value it gives; one against another clock or edge
  replaces all delays of the port, unless it has `-add_delay` (the port is then timed against each).
- The ports: `get_ports`, `all_inputs`, `all_outputs`, plain port names, or the pad buffer cells
  (`get_cells`). An input delay on an output port (and the reverse) is ignored with a warning.
- `-clock` takes the name of a clock (`create_clock -name`, or the net it is on) or `get_clocks`. It has to be
  the clock of the registers on the paths: behind a PLL that is the PLL's output clock (by the name of its net;
  the packer constrains it, so it is not known to `get_clocks` when the SDC file is read, and is given as a
  plain name).

In the analysis:

- The start points of such an input and the end points of such an output are clock domains of their own (the
  clock edge at the clock pad). Their paths count like the paths between registers: slack and criticality for
  the placers, the router and the repairs, Fmax of the clock (a failing output delay is a failing clock), the
  critical path reports, the slack histogram, TNS, the hold checks in all hold corners and the hold fix.
- The registers are behind the clock network: on a path from an input the capturing register sees the edge
  later by the **clock latency** (from the clock pad through the pad buffer, the clock input and the global
  buffer, with the routing between them, to the clock net) and the routing of the clock net to its clock pin. On
  a path to an output, the launching register does. Between two registers, and from an input to an output,
  the latency is on both sides and not counted. It is followed back from the clock net as far as the clock is
  the same: it ends at the pad, or where another clock comes in (the reference input of a PLL, so the phase of a
  PLL clock against the board clock is not in it).
  Sizes on a CCGM1A1 at `worst_spd`: 4.9 ns from a clock pin (`IO_SB_A8` …) to the clock net and 1.2 ns more to a
  register; 8.4 ns and 1.6 ns for a clock that comes in by an ordinary pin and reaches the global buffer through
  the fabric.
- These paths have no part of the clock network in common with the clock of the device outside, so the latency
  counts with its least or its greatest value in the corner, whichever is against the check: the greatest for
  the hold of an input and the setup of an output, the least for the setup of an input and the hold of an
  output. (The routing of the clock net to the register counts with one value, the greatest, as everywhere.)
- The latency helps the setup of inputs and costs it at outputs: a register's clock-to-output time at the pad is
  latency + clock routing + clock-to-Q + routing + output buffer, 10 ns and more. It works against the hold of
  inputs: data that changes soon after the edge at the pad is too early for a register that sees the edge 6 ns
  later. The hold fix pads such inputs with delay cells; a register in the I/O cell (`FF_IBF`, `FF_OBF`) keeps
  the data path short where that is not wanted.
- The pad buffers count on these paths (0.5 ns input, 1.0 ns output at `worst_spd`), which they do not on
  unclocked ones.
- The delays outside of the device are the same in every timing corner; the latency, the buffers and the paths
  are those of the corner.
- Multicycle and false paths apply as to other paths of the clock (`-from [get_ports …]`, `-to [get_ports …]`,
  or `get_clocks`: a false path between two clocks takes their I/O paths too).

The timing report lists the delays per command, with the worst slacks of their paths (an error like a failing
clock, where the report raises those):

```
Info: Input and output delays (the paths from the inputs and to the outputs):
Info:     set_input_delay -clock sys -max 4.0 [get_ports {din[*]}]; set_input_delay -clock sys -min 1.5 [get_ports {din[*]}]
Info:         16 port(s): worst setup slack 7.40 ns at 'din[3]', worst hold slack 0.12 ns at 'din[9]'
```

Path reports have the segments `in-delay` and `out-delay`, the pad buffer (`source` after the one, `logic`
before the other) and the clocks as `clk-skew` (negative on a path from an input: the capturing register's
clock is later).

A delay that can not be applied is a warning in the timing report, and its port stays unclocked: a clock that
does not exist or clocks no register, a port that no path starts or ends at.

Limits: no `-rise`/`-fall`, `-reference_pin` or latency options; no virtual clocks (`create_clock` without
objects) and no `set_clock_latency`: a delay names a clock of the design, and what differs outside goes into its
value. Before routing the latency is an estimate (the placers see a different value than the final analysis).
The delays are not written to the SDF file or kept in a saved design: the SDC file is read again.

### Against a clock the design puts out

A device that gets its clock from the FPGA (a source synchronous interface: a memory, a converter, an SPI
slave) sees the clock edge when it leaves the FPGA at that port, not when it comes in at the clock pad.
`create_generated_clock` on the output port names that clock, and the delays are given against it:

```tcl
create_clock -name sys -period 10 [get_ports clk]
create_generated_clock -name fwd -source [get_ports clk] [get_ports clk_out]
# the device: setup 2.0 ns, hold 0.5 ns; the data traces are 0.2 ns longer than the clock trace at most
set_output_delay -clock fwd -max  2.2 [get_ports {dout[*]}]
set_output_delay -clock fwd -min -0.5 [get_ports {dout[*]}]
# its data back: clock-to-output 1.0 to 4.0 ns, plus the clock trace to it and the data trace back (0.5 ns each)
set_input_delay -clock fwd -max 5.0 [get_ports {din[*]}]
set_input_delay -clock fwd -min 2.0 [get_ports {din[*]}]
```

- The values are as above, with the clock trace from the FPGA to the device in place of the difference of the
  clock traces: for an output the data trace less the clock trace, for an input the clock trace plus the data
  trace.
- The analysis follows the clock back from the port: through its pad buffer, the routing and the cells it
  passes (the input that carries a clock), to a clock net of registers or to the clock network before it. The
  delay of that way is where the edge at the port is after the edge at the clock net, and takes the place of
  the clock latency: the register at the other end of a path is *earlier* than the edge at the port by it. The
  clock network from the clock pad is common to both and not counted. The delay counts with its least or its
  greatest value, whichever is against the check: the least for the setup of an output and the hold of an
  input, the greatest for the hold of an output and the setup of an input.
- The clock may be put out by a register that puts out both of its clock edges: `CC_ODDR` with the data 1 and 0
  (or 0 and 1 with `-invert`). That is the shorter and steadier way: on a CCGM1A1 at `worst_spd` the edge at
  the port is 1.6 to 1.7 ns after the clock net, against 3.3 to 3.6 ns for `assign clk_out = clk`, which goes
  through the fabric (routed values of a small design; before routing the estimate of the fabric way was twice
  that).
- `-invert`: the rising edge at the port is the falling edge of the clock of the registers. Only a clock of the
  same frequency is supported (`-divide_by 1`, `-multiply_by 1` are accepted; a port driven by an ordinary
  register is a divided clock and not followed). `-source` is accepted and not used: the way of the clock is
  taken from the design. One clock per port; the name is that of the port without `-name`.
- The way has to meet the clock net of the registers, or the clock network before it. A clock put out from
  another output of a PLL than the one that clocks the registers (a phase shifted copy) does not: the phase
  between PLL outputs is not in the analysis, and delays against such a clock are not applied.
- `get_clocks` finds the clock, also in `-to`/`-from` of the exceptions (the paths against it; they are also
  paths of the clock of the registers and are taken by an exception for that one).
- From an input to an output that are both against this clock, its way is on neither side. With one of them
  against the clock at its pad, both ways are between the two edges.
- The hold of such outputs is short when the clock's way to its port is longer than the data's way to theirs.
  The hold fix pads these paths. Padding that is long enough in the fastest corner is about twice as long in
  the slowest one, so a narrow window (`-max` less `-min` close to the period, or a late clock) can leave no
  setup slack for it: the fix then leaves the path and says so (`no setup budget`). Putting the clock out by a
  `CC_ODDR` and the data by registers in the I/O cells keeps the two ways alike.

The timing report names the clock with its delay, before the delays against it:

```
Info: Input and output delays (the paths from the inputs and to the outputs):
Info:     clock 'fwd' at the port 'clk_out': 3.28 to 3.56 ns after the clock net '$iopadmap$clk'
Info:     set_output_delay -clock fwd -max 2.0 [get_ports {dout[*]}]; set_output_delay -clock fwd -min -1.0 [get_ports {dout[*]}]
Info:         8 port(s): worst setup slack 13.72 ns at 'dout[3]', worst hold slack 1.06 ns at 'dout[5]'
```

A delay against a clock whose port puts out no clock of registers is a warning, and not applied.

## In the flow

The timing analyser gives the start and end points that constraints name their own clock domains. Every
domain pair has one multicycle setting (and is a false path or not), and it is used for:

- the slack and the criticality that the placers, the router and the repair passes work with. The pairs of one
  launch/capture clock edge are scaled together, so a multicycle path is as critical as the time it has;
- Fmax (a path with `n` cycles counts at `n` times the clock's frequency), the slack histogram and TNS;
- the hold checks after routing and of the finished routing, and the Paths tab of the GUI. Path reports say
  `multicycle path: …`.

Multicycle paths are seldom the critical paths of their clock, so the timing report lists them on their own:
per constraint and clock edge pair the number of end points, the worst setup slack and the worst hold slack with
the shortest path (`Multicycle paths 'negedge clk' -> 'posedge clk' (2 clock cycle(s) for setup, …)`).

The RAMs are replaced by new cells after placement (two halves of a block become `$ram$merged$id…`, a single
one `…$full`); a constraint naming a RAM or one of its pins then names the new cell's ports of that half.

Without timing exceptions the analysis is the same as before.

## Hold checks

The finished routing (what goes into the bitstream) has its hold checked in every corner of the performance
mode: best, typ and worst (`best_spd`, `typ_spd`, `worst_spd` with `fpga_mode=speed`). The performance mode
(VCORE) is a board decision, not a variation, so the other modes are not checked unless asked for.

- `--vopt hold-corners=all` (default), `routed` (only the speed grade the design was placed and routed for, the
  check of before; quicker for tests) or a list as for `timing-corners` (`best,typ_eco`). The routed grade is
  always checked, its Fmax is reported and it stays the timing result; of the other corners only the hold
  violations are reported (`timing-corners` gives their full reports).
- A violation in any corner is an error unless `--timing-allow-fail`; it is raised after all corners are
  reported, naming the failing ones (`Hold violations in timing corner(s) 'worst_spd', 'best_spd'`). Each
  corner logs `Hold check in timing corner '…': N violation(s), worst hold slack … ns`.
- The router's own timing report, before the repairs and the hold fix, lists the hold violations it leaves
  (`N Hold/min time violations (showing 10 worst paths), before the repairs and the check of the finished
  routing`) without raising them: with input delays they are the rule there (see the clock latency), and the
  hold fix repairs them.
- The hold analysis takes the least routing delays of the speed grade: per pip the lesser of the min values of
  the vendor's delay files (rise and fall), about 12 % below the max values the routers and the setup analysis
  use. Setup results, placement and routing do not change. `--vopt hold-min-delays=0` gives hold the max values
  again (for comparisons). What min, typ and max within one corner's delay file mean physically is not
  confirmed by Cologne Chip; the min is taken as the conservative choice.
- Routing into clock pins counts with one value (the max), as for setup: without common path pessimism
  removal, the least delay to the launching and the greatest to the capturing register would make up a skew the
  shared part of the clock tree does not have.
- The hold slack is exact at every port of a path, not only at the end points (the hold part of the required
  time goes back by the least delays), for the hold fix (`TimingAnalyser::get_hold_slack`,
  `get_absolute_setup_slack`).

## Hold fixing

After the setup repairs (the last pass of `repairRoute`, before the timing corners and `finishRoute`), connections
whose hold slack is below a margin in any hold corner get delay cells (see below), where the setup slack allows
(`hold_fix.cc`).

- `--vopt hold-fix=0|1` (default 1). With nothing below the margin the pass changes nothing (no cell, the routing as
  it was) and costs only the analysis of each corner.
- `--vopt hold-margin=<ns>` (default 0.1): every connection whose hold slack is below it in any hold corner is
  repaired up to it. The sign-off check of the finished routing stays at 0. The margin covers what `finishRoute`
  changes: bridges, CP lines and pass-through LUTs become cells whose delays differ a little from the pips they were
  analysed as (tens of ps: on dmg_soc the worst hold slack fell by 0.03 / 0.04 / 0.06 ns in best / typ / worst
  from the routing the pass sees to the finished one).
- `--vopt hold-fix-max-cells=<n>` (default 8): the most delay cells in a chain on one connection; a connection
  needing more is reported, not filled.
- `--vopt hold-fix-radius=<tiles>` (default 6): how far from the sink a free CPE is searched for.
- `--vopt hold-fix-setup-guard=<ns>` (default 0.1): the setup slack a lengthened path keeps at least.
- The corners: hold is repaired in the routed grade and every corner of `hold-corners` (the same corners the
  sign-off check covers); setup is protected in the routed grade only (where Fmax is defined).

How it works, in rounds (at most 12):

1. Every corner is analysed exactly (its speed grade selected; the routed grade is selected again at the end).
2. Per end point below the margin (the worst first, by name on a tie): its shortest path, and on it the connection
   nearest to the end point whose setup slack in the routed grade, less the guard, covers the delay the cells add
   (their greatest delay and the detour through their CPEs, less the old connection; estimated with the delay
   tables, verified exactly afterwards). A connection whose setup slack is too small (or negative: a failing path is
   never made slower) sends the search upstream along the path. That also resolves an end point reached by
   several launch domains (multicycle tags) where one needs delay and another has no setup slack to spare on the
   connection they share: the cells go on the needy path's own connections. In one round no path gets two
   padded connections, and a connection shared by several end points is padded once. Connections inside one CPE
   (a LUT into its register), of clock and prerouted nets and into 8:1 multiplexers are left out.
3. How much: the smallest chain (one cell of tap 0, 1 or 2; then k - 1 cells of tap 2 and one of any tap) whose
   least delay in every corner, with the estimated change of the routing scaled to the corner, covers the end
   point's need in that corner. The next round adds more where the estimate fell short.
4. Where: the free CPE (`delay_cell_fits`) within the radius around the sink with the least estimated detour.
5. All plans of a round are inserted and routed together (`route_ripped`); if that fails, all is undone and each
   plan is tried alone (a plan that still fails is undone and reported). Then all corners are analysed again; a
   plan is undone if the setup slack at its sink fell below half the guard or its hold slack did not rise. If the
   setup result as a whole got worse (TNS; Fmax of a clock that failed its constraint, or a clock that met it no
   longer meeting it), the whole round is undone. Undone plans' connections are not tried again.
6. It stops when nothing is below the margin, a round finds nothing to do, or after the last round. The routing is
   checked complete and consistent at the end.

The log has per corner the worst hold slack and the end points below the margin before and after, the cells by
tap, and each end point left below the margin with the reason: no setup budget (with the need and the largest budget
on its path), no free CPE nearby, not routable, no gain (the new routing faster than the old), the chain cap (with a need of the order of a clock period, or where a
setup multicycle moved the hold check without `-hold`: "a `set_multicycle_path -hold` is probably missing"). Left
below 0 they are warnings here; the error comes from the sign-off check.

Limits:

- A path that keeps more setup slack than the guard may become slower: the worst setup slack of a clock that meets
  its constraint can fall to the guard (it still meets it); Fmax of a failing clock and the TNS do not change.
- False paths (`set_false_path`, `set_false_path -hold`) are not padded. A delay cell on a connection that a
  false path shares with a timed path lengthens the false path too.
- Paths from inputs with a minimum input delay and to outputs with a minimum output delay are padded like any
  other. The connections between a pad and its I/O cell are fixed wires: the cells go on the routing in the
  fabric next to them. An input or output without a minimum has no hold check and is left alone.
  Each end point gets a chain of its own, so an input into several registers costs several chains. On
  `tools/gui_tests/designs/multicycle.v` with `io_delays.sdc` (16 inputs into four registers each, minimum input
  delay 1 ns): 98 delay cells with the clock on a clock pin, all corners clean; with the clock on an ordinary pin
  (8.4 ns latency) 142 cells and one end point left without a free CPE within the default radius, 145 cells and
  all clean with `--vopt hold-fix-radius=12`.
- No clock-side fixes and no routing detours as padding; a design routed before is skipped (one log line).
- A delay cell's delay grows from best to worst corner by about 2.2x, more than the two periods of a setup
  multicycle do: a setup multicycle without `-hold` on a short path is often unfixable in all corners at once.

## In the GUI

- **Paths** tab: below a selected path, the **Slack** table shows how its slacks come about. For setup: the
  required time (the capture edge from the clock constraint, the cycles a multicycle path adds, the clock delay
  to the capturing register, clock to clock, the setup time) against the arrival time (the clock delay to the
  launching register and the data path); for hold the same with the hold edge, the hold time and the shortest
  path. Each row says where it comes from (the SDC line of an exception, the register). Below that, the
  constraints on the path: the clocks with their constraints and every exception whose lists match it, with
  what it does (applies to setup or hold, overridden by line N, a false path taking a check away). A check
  that a false path takes away shows `-` as its slack, with the times it would have.
  The paths from inputs with an input delay and to outputs with an output delay are listed with the clock they
  are timed against. Their Slack table has the delay outside of the device (`Input delay` in the arrival time,
  `Output delay` taken from the required time; the maximum for setup, the minimum for hold), the clock network
  from the clock pad to the register at the other end, and the SDC commands under the constraints; a check that
  a missing `-max` or `-min` leaves out shows `-`. Against a clock the design puts out, the rows name its port
  as the place of the clock edge (the register's clock delay is then negative: its edge is before that at the
  port), and the constraints list the `create_generated_clock` command too.
- **Exceptions** tab: every timing exception of the SDC file with its line, the number of paths (end points per
  launch clock) its lists match, the number it applies to and the worst setup slack among them (`-` for a
  false path: its paths have none). Below, the
  paths of the selected exception (or of all), with what the exceptions do to each; a selected path is selected
  in the device view.

## Delay cells

For lengthening a connection with a hold violation, one CPE can be a pure delay from IN6 to an output
(`CPE_DELAY`, `delay_cell.cc`): tap 0 through the LUT tree to OUT1, tap 1 through the carry select of the two-bit
adder to OUT2, tap 2 from there through COUTX and back through CPOUT1 to OUT1; 0.29, 0.34 and 0.71 ns at best_spd,
0.57, 0.67 and 1.40 ns at worst_spd (maximum). A timing exception naming the split net as an end point
(`-to [get_nets …]`) is extended to the cell's output net. The path analysis, the configurations, the delays per
corner and what is validated how: [delay_cells.md](delay_cells.md).
