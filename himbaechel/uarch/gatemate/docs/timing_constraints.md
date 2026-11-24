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

Without multicycle constraints the analysis is the same as before.
