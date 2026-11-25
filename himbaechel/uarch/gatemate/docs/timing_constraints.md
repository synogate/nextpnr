# Timing constraints (SDC)

The timing constraints come from an SDC file (`--sdc top.sdc`). It is read before packing, so object names
are those of the synthesised netlist (as in the JSON file).

## Commands

| Command | Notes |
|---|---|
| `create_clock -period <ns> [-name <name>] <objects>` | on nets, cell pins or ports. The name (else the net's name) stays with the constraint when the packer copies it behind a clock buffer, and `get_clocks` finds the clock by it. |
| `set_multicycle_path <n> [-setup\|-hold] [-start\|-end] [-from <objects>] [-to <objects>]` | see below |
| `set_false_path [-setup\|-hold] [-from <objects>] [-to <objects>]` | recorded and shown in the GUI with the paths it covers, but not applied by the timing analysis yet |
| `get_cells`, `get_pins`, `get_nets`, `get_ports`, `get_clocks` | names or patterns with `*` and `?` (brackets are bus indices, `{a[3]}`) |

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
(asynchronous start and end points) are not timed against a clock, so constraints on them have no effect.

## In the flow

The timing analyser gives the start and end points that constraints name their own clock domains. Every
domain pair has one multicycle setting, and it is used for:

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

Without multicycle constraints the analysis is the same as before.

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
- The fixer knows no false paths (`set_false_path` is not applied by the analysis yet): a false path below the
  margin is padded like any other.
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
  what it does (applies to setup or hold, overridden by line N, a false path that is not applied).
- **Exceptions** tab: every timing exception of the SDC file with its line, the number of paths (end points per
  launch clock) its lists match, the number it applies to and the worst setup slack among them. Below, the
  paths of the selected exception (or of all), with what the exceptions do to each; a selected path is selected
  in the device view.

## Delay cells

For lengthening a connection with a hold violation, one CPE can be a pure delay from IN6 to an output
(`CPE_DELAY`, `delay_cell.cc`): tap 0 through the LUT tree to OUT1, tap 1 through the carry select of the two-bit
adder to OUT2, tap 2 from there through COUTX and back through CPOUT1 to OUT1; 0.29, 0.34 and 0.71 ns at best_spd,
0.57, 0.67 and 1.40 ns at worst_spd (maximum). A timing exception naming the split net as an end point
(`-to [get_nets …]`) is extended to the cell's output net. The path analysis, the configurations, the delays per
corner and what is validated how: [delay_cells.md](delay_cells.md).
