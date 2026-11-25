# Timing constraints (SDC)

The timing constraints come from an SDC file (`--sdc top.sdc`). It is read before packing, so object names
are those of the synthesised netlist (as in the JSON file).

## Commands

| Command | Notes |
|---|---|
| `create_clock -period <ns> [-name <name>] <objects>` | on nets, cell pins or ports. The name (else the net's name) stays with the constraint when the packer copies it behind a clock buffer, and `get_clocks` finds the clock by it. |
| `set_multicycle_path <n> [-setup\|-hold] [-start\|-end] [-from <objects>] [-to <objects>]` | see below |
| `set_false_path -from … -to …` | accepted, no effect yet |
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
