# Placing and routing by hand in the GUI

The Qt GUI (`--gui`) can place and route a design, or parts of it, completely by hand, mixed freely with
the Pack / Place / Route passes. It is developed and tested with himbaechel/GateMate; the command layer is
architecture independent.

```
nextpnr-himbaechel --device CCGM1A1 --json design.json -o ccf=board.ccf --gui
```

Load a synthesized JSON and run **Pack** (then place by hand), or load a placed / routed JSON (e.g. one
written by `--write`) and edit it.

## Basics

- **Edit mode** (`E`, Edit menu, toolbar) must be on for any change; it is refused while a pass runs.
- Every change is one entry on the undo stack: **Undo** `Ctrl+Z`, **Redo** `Ctrl+Y` / `Ctrl+Shift+Z`. The
  stack is cleared when a pass starts or a design is loaded.
- Hovering an element shows what it is (bel, wire, pip, cell, net, state, delay). Where elements overlap,
  click again or press `Tab` to cycle through the candidates under the cursor (smallest first);
  right-click opens a "Select ..." list. The status bar always shows the selection.
- View: wheel zooms (20 % per notch, around the cursor), `Shift`+drag pans. Window layout, docks and
  splitters are kept between sessions (`NEXTPNR_GUI_SETTINGS=<file.ini>` uses another settings file).
- Refused edits explain why in the status bar (occupied bel, location rules, fixed cells, ...); refused
  context-menu entries are greyed out with the reason as tooltip.

## Placement

| Action | How |
| --- | --- |
| Arm a cell | Select it in the Cells tree (edit mode on), or `P` for the selected cell / the cell on the selected bel. Green / orange / red marks where it may / may not go. `Esc` cancels. |
| Place / move / swap | Click a bel while a cell is armed. A whole cluster (e.g. LUT + register) moves with its root. Clicking a full-CPE bel takes the matching half. Clicking a bel of another (unlocked) cell swaps the two. |
| Drag and drop | Drag a placed cell (more than 6 px) onto another bel. |
| Move several cells | Select them, **Move selection by...** (`M`) or the arrow keys in the view. |
| Next unplaced cell | `N` (arms it and centres the view); the Cells tab has an "Unplaced only" filter. |
| Bel context menu | Place here, Move, Unplace, Lock (USER) / Unlock (STRONG), Show in tree. Cells tree: Unplace. |
| Unlock | Edit > Unlock selected cells / Unlock all cells / Unlock all routing. |

Cells placed by hand get STRONG: the placer may still move them. Locked cells (USER and above) are never
displaced by a swap and are kept by Place. Cells FIXED by the packer (GateMate: the CPEs tied to IOs, RAMs,
clocking, CP-line cells) cannot be moved.

Unplacing or moving a cell of a routed design also unbinds its CPE-internal routing and the routing of the
nets it drives. On GateMate, inputs that the routing took from carry / propagate lines (CP lines, which only
reach the old CPE) go back to the normal CPE inputs and to the net the CP lines passed on, so **Route**
can route the cell at its new place. All of it is one undo entry. A design loaded from JSON has no clusters:
move a LUT together with the register it feeds inside its CPE (select both, `M`), as a register's data input
is only reachable from its own CPE.

## Routing

| Action | How |
| --- | --- |
| Look at a net | Select it (Nets tree or a wire): its tree is drawn, source magenta, sinks cyan (routed) / yellow (open), open arcs as straight ratsnest lines. The "Routing" properties list every arc with status and delay. |
| Show all open arcs | View > Show unrouted arcs (`U`). |
| Rip up | Context menu of a wire / pip / net: Unroute net, Unroute arc (to one sink), Remove pip. The source wire stays. |
| Route by hand | **Route net by hand** (`R`) on the selected net / wire. The head wire is shown with the pips downhill of it: green = free, orange = usable with conflicts, grey = not allowed. Click a pip to bind it (STRONG) and continue from its end; `Backspace` takes the last pip back, `Esc` ends. Right-click lists the candidates. |
| Through a CPE | (GateMate) Every pip inside a CPE has a stretch of its own to click on. The carry and propagate (CP) lines: from OUT1 / OUT2, right of the outputs, into the carry logic (the CARRY block, one row per line: CY1, PY1, CY2, PY2 at the top go up to COUTY1 ... POUTY2, CX and PX at the bottom down to COUTX / POUTX on their bands), on into the next CPE's CINX / CINY..., and from there through the carry logic again or into a LUT2 input (C_I1..C_I4), the clock or the enable. |
| Shortest path | In route mode `Ctrl`+click a wire: shortest free path from the head to it (waypoints). |
| Route candidates dock | View > Route candidates: the same as lists (also for wires without graphics), the session's pips, the arcs of the net (double-click routes to that sink), "Route to" a wire by name, **Route arc** / **Route rest of net**, lock / unlock. |
| Lock routing | Lock (USER, drawn thick orange) keeps it through Route; Unlock sets STRONG. |

## Passes, checks, timing, saving

- **Pack**, **Place**, **Route** (Design menu) can be used whenever they make sense: Place while cells are
  unplaced (placed cells keep their bels), Route again and again after edits (only what is missing or
  ripped up is routed; locked routing stays). A failed pass does not end the GUI.
- **Design check** dock (View menu): unplaced cells, open / partial arcs, conflicts, dangling routing,
  invalid locations. Double-click zooms to the element.
- **Timing** dock (`T` runs the analysis): Fmax per clock and the critical path on the design as it is
  (unplaced cells left out, open arcs with estimated delays); "Highlight critical path" draws it.
- **File > Save JSON** writes the design at any stage (packed, partly placed, partly routed, with the locks)
  and **File > Open JSON** (or `--json <file> --gui`) continues it where it was: the placement, routing and
  strengths are attributes of the cells and nets, and what the GateMate packer worked out (clusters, regions,
  multiplier and partition roles, the partition settings) is saved with them, so a packed design is not packed
  again. Open JSON replaces the design in the session (the edit history starts empty). Without the GUI,
  `--json <file> --no-pack` places and routes a saved design further. Routing that a Route left open for the
  repairs is finished before saving. The first Place keeps the cells that hand routing reaches. **File > Save as
  partition** (session started with `--vopt as-partition`) writes a GateMate partition to import with
  `--vopt partition=<instance>:<file>`. The bitstream is written by Route when `-o out=<file>` was given.

## Changing the logic by hand

Good routing sometimes needs other logic: two LUTs merged into one tree, an MX8 instead of MX4s, the L30
comparator LUT2 of a CPE, inputs through the CP lines. The netlist can be edited in the GUI, and a logic check says
whether the design still computes what it did.

- **Logic** submenu of a cell's context menu (Cells tree, edit mode): **New cell like X** (same type, ports and
  parameters, unconnected, armed for placement), **Set parameter** (`NAME=VALUE`, e.g. `INIT_L10=0110`; `0x..`
  hexadecimal, binary as the property view shows it, decimal, else a string; an empty value removes it), **Connect
  pin** (`PIN NET`: the pin is added as an input if the cell lacks it, the net is created if there is none),
  **Disconnect pin**, **Remove cell** (unplaced with its routing and disconnected first). An empty net can be removed
  from its context menu. Every edit is one undo step; disconnecting removes the routing that served the pin.
- **Logic reference**: a copy of the netlist the check compares with. It is taken after Place when there is none
  (GateMate packing ends there: repack merges the halves of a CPE), from a placed design when it is loaded, by hand
  with **Logic > Take logic reference**, and is saved next to the design (`<name>.logicref.json`, written by Save
  JSON, read by Open JSON). Logic > Save / Load logic reference for other files, Clear to drop it.
- **Logic check** (Design check dock, line "Logic: ..."): every register input (data, clock, enable, set, reset as
  they take effect), RAM, IO and other non-logic cell input is compared between design and reference, as functions
  of the register / RAM / input outputs (matched by cell and pin names). Both netlists are simulated with random
  values; a difference is listed under "Logic differs from the reference" with a counterexample (the values of the
  nets around the change). Then each sink is proven equal through the cones back to nets already proven, which
  covers unchanged logic in one step per cell and changed logic as a whole up to 16 inputs; what is only simulated
  is listed separately. Removed or new registers and free variables the reference does not have are listed too.
  The check runs after every edit while it takes less than a second, else with Refresh / Logic > Check logic now.
  It compares the logical netlist: GateMate's routing renames (LUT inputs on the pins the routing reached, carry
  line inputs, CP-line cells) are followed, but a routed path itself is not checked.

## Shortcuts

| Key | Action |
| --- | --- |
| `E` | Edit mode on / off |
| `P` | Arm the selected cell for placement |
| `N` | Next unplaced cell |
| `M` | Move selection by... (arrow keys: nudge in the view) |
| `R` | Route the selected net by hand |
| `U` | Show unrouted arcs |
| `T` | Run timing analysis |
| `Esc` | Cancel placement / end routing |
| `Backspace` | Route mode: take back the last pip |
| `Tab` | Cycle the candidates under the cursor |
| `Ctrl+Z` / `Ctrl+Y` | Undo / Redo |

## A typical session

1. Load the synthesized JSON with the constraints, **Pack**, `E`.
2. Place the critical cells by hand (`N`, click; drag to adjust), lock them. **Place** the rest.
3. Route critical nets by hand (`R`, clicks / `Ctrl`+click, or Route arc in the dock), lock the routing.
4. **Route** the rest; check the Design check and Timing docks; move cells / rip up and Route again.
5. Save the JSON (or the partition); the bitstream comes with `-o out=...`.

## GUI tests

The GUI is tested by scripts run on an invisible desktop (nothing appears on the screen); see
`gui/guitest.h` for the script commands. All tests (designs from prjpeppercorn-test-cases, built once into
the output directory):

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\gui_tests\run_all.ps1 -OutDir C:\tmp\gt
powershell ... -File tools\gui_tests\run_all.ps1 -OutDir C:\tmp\gt -Only move_routed
```

Each script names its design in a `# design:` line; results (log, screenshots, saved JSONs) go to
`<OutDir>\<script>\`. Two tests write designs that are then checked without the GUI:

```
powershell ... -File tools\gui_tests\check_flow.ps1 -Dir C:\tmp\gt\flow_5a          # routing trees, gmpack, eqy
powershell ... -File tools\gui_tests\check_partition.ps1 -Dir C:\tmp\gt\partition_5b # import the partition
```

A single script: `tools\run_gui_test.ps1 -Json <design.json> -Script <script.txt> -OutDir <dir>
[-ExtraArgs "-o ccf=..."]`. `tools/gui_tests/check_routed.py <file.json>` checks that every net of a saved
design is one routing tree.
