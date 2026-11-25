# Delay cells

A delay cell is one CPE configured as a pure delay: one routed input (IN6), one output, the identity in between.
The hold fix (`hold_fix.cc`, [timing_constraints.md](timing_constraints.md) "Hold fixing") puts one or a chain of
them into a connection with a hold violation (`delay_cell.cc`, API below). The cell has the
type `CPE_DELAY`, takes the whole CPE (bel `CPE_LT_FULL`) and comes in three taps, each a different configuration.

## The paths through a CPE

The timing database of the vendor has, per mode of the CPE, an arc from each input to OUT1 / OUT2 (`_ARBLUT_*`, the
plain LUT tree; `_ADDF2Y1_*` / `_ADDF2X_*`, the two-bit adder; `_MX4A/B_*`; `_MX8_*`; `_MULT_*`) and the pieces of
the carry / propagate lines (`comb12_*`, `CPOUT_cpout`, `cpout_OUT1/2`, `_ROUTING_*`), which the chip database also
uses as the delays of the CPE's pips. The longest arcs from a CPE input to an output (best_spd, ns, max of rise/fall):

| Arc | Path | max |
|---|---|---|
| `_MX8_CLK_OUT2` | CLK through the select adder of the 8:1 mux | 0.358 |
| `_ADDF2Y1_IN6_OUT2` | IN6 -> C_I3 -> L02 -> carry select of ADDF2 -> COMB2OUT -> OUT2 | 0.343 |
| `_ADDF2Y1_IN5_OUT2` | the same from IN5 | 0.331 |
| `_ARBLUT_IN2_OUT2` | IN2 -> L00 -> L10 -> COMB2OUT -> OUT2 | 0.316 |
| `_MX4B_IN2_OUT1` | IN2 -> L00 as a select of the 4:1 mux -> L20 -> OUT1 | 0.312 |
| `_ARBLUT_IN6_OUT1` | IN6 -> L02 -> L11 -> L20 -> COMB1OUT -> OUT1 | 0.285 |
| `_MX8_IN6_OUT1` | IN6 -> 8:1 mux (bridge) -> OUT1 | 0.185 |

The 8:1 mux paths are no identity with one input: a select passes one of two data inputs, which would need constants
on other inputs (routed), and its data inputs cannot undo an inversion of the routing. So the longest single arc that
is an identity of one input is the carry select of the two-bit adder (`C_FUNCTION = C_ADDF2`): COMB2OUT = L10 ^ (L11 ?
carry in : L02); with L10 and L11 constant 0 it is L02, which passes IN6.

From OUT2 the CPE can go on: OUT2 is one of the inputs of the routing multiplexers of the carry lines, and the carry
lines come back into OUT1 / OUT2 through CPOUT1 / CPOUT2. OUT2 -> CX_OUT ({C_SEL_C, C_SELX} = 01) -> COUTX (C_CX_I = 1)
-> CP_CARRY (C_HORIZ = 1) -> CPOUT1 (C_C_P = 1) -> OUT1 (C_O1 = 2) adds `comb12_COUTX + CPOUT_cpout + cpout_OUT1`,
0.36 ns at best_spd. COUTX is the longest of the six routes back (`comb12_COUTY1` 0.208, `comb12_POUTX` 0.205; through
the comparator, COMB -> L30 -> COMPOUT -> COUTX, is shorter than through OUT2). Only one such round trip fits in one
CPE: the other output would have to feed it back.

**The hint** ("a row of input multiplexers over 4 LUT2, into the carry select logic and back out of the carry select
logic to OUT1/2") is right: the longest identity path enters through the input multiplexer C_I3 (TI6) into one of the
four first-level LUT2 (L02), goes through the carry select of the two-bit adder, and comes back out through the
selects of the carry lines (C_SELX / C_SEL_C / C_CX_I, COUTX) and CPOUT1 to OUT1. It does not invert.

## Taps

All taps: input IN6 (C_I3 = 0), `INIT_L02 = 1100` (passes IN6; written `0011` where the route to IN6 inverts, as
for any LUT input), `INIT_L00 = INIT_L01 = INIT_L03 = INIT_L10 = 0000`.

| Tap | Output port | Configuration | Path |
|---|---|---|---|
| 0 | OUT1 (COMBOUT1) | `C_FUNCTION 000`, `INIT_L11 1010`, `INIT_L20 1100`; C_O1 = 3 from the routing | L02 -> L11 -> L20 -> COMB1OUT |
| 1 | OUT2 (COMB2OUT) | `C_FUNCTION 010` (ADDF2), `INIT_L11 0000`, `INIT_L20 0000`; C_O2 = 3 from the routing | L02 -> carry select -> COMB2OUT |
| 2 | CPOUT1 | tap 1 and `C_O2 11`, `C_SELX 1`, `C_SEL_C 0`, `C_CX_I 1`, `C_HORIZ 1`, `C_C_P 1`; C_O1 = 2 from the routing | tap 1 -> OUT2 -> COUTX -> CPOUT1 -> OUT1 |

Delays (`getCellDelay`, ns; rise min/max, fall min/max; the analysis takes min = the smaller minimum, max = the larger
maximum). Tap 0 is `_ARBLUT_IN6_OUT1`, tap 1 `_ADDF2Y1_IN6_OUT2`, tap 2 `_ADDF2X_IN6_OUT2 + comb12_COUTX + CPOUT_cpout
+ cpout_OUT1`:

| Corner | Tap 0 | Tap 1 | Tap 2 |
|---|---|---|---|
| best_spd | 0.238/0.273, 0.243/0.285 | 0.265/0.327, 0.270/0.343 | 0.597/0.689, 0.605/0.708 |
| typ_spd | 0.324/0.376, 0.326/0.394 | 0.369/0.453, 0.371/0.463 | 0.836/0.956, 0.841/0.970 |
| worst_spd | 0.476/0.550, 0.470/0.573 | 0.547/0.654, 0.540/0.670 | 1.222/1.379, 1.222/1.401 |
| best_lpr | 0.330/0.381, 0.336/0.399 | 0.374/0.453, 0.378/0.480 | 0.843/0.965, 0.853/0.997 |
| worst_lpr | 0.907/1.046, 0.881/1.087 | 1.071/1.255, 1.046/1.275 | 2.376/2.662, 2.369/2.686 |

The connection the cell is put into gets longer by more than the tap: the routing to the CPE and from it.

## What the cell takes

The whole CPE: no other cell may be in it (`isBelLocationValid`) except the CP-line cells finishRoute makes for other
nets' routing through it; the CPE is marked used (no bridge through it). The registers are not usable: their data
comes from the LUT tree, which is the cell's. Tap 0 alone only needs the lower half, and the upper half could be a
second tap 0 (a plain LUT1 buffer in each half), but a cell of a fixed input pin and configuration needs a bel of its
own; that is not done. Taps 1 and 2 need the whole CPE (C_FUNCTION is one for both halves), tap 2 also its carry
line selects and OUT2, whose output multiplexer (C_O2) no net may take (`checkPipAvail`).

The other inputs of the CPE are not routed and carry whatever their input multiplexers select: the configurations do
not depend on them. Other nets may still pass through the CPE's carry lines (CINX -> COUTX and the like), with any
select for taps 0 and 1; for tap 2 only with the selects as it sets them (`delay_cell_fits` checks the routing).

## API (GateMateImpl, `delay_cell.cc`)

- `std::vector<DelayQuad> delay_cell_taps() const` — the delay of each tap for the speed grade selected
  (`select_speed_grade`); `bool delay_cell_delay(int tap, DelayQuad &)` for one.
- `bool delay_cell_fits(BelId bel, int tap) const` — the CPE of `bel` is free for a cell of that tap now.
- `CellInfo *insert_delay_cell(NetInfo *net, const PortRef &user, BelId bel, int tap)` — a cell `hold$delay$<n>`
  between `net` and its sink `user`, placed (STRENGTH_FIXED), with a new net `hold$delay$<n>$out` to the sink; the
  route of `net` to that sink is released, the two connections are left unrouted (`route_ripped()` routes them).
  (No `$` in front of the names: the JSON frontend renames such cells on loading.)
- `bool remove_delay_cell(CellInfo *)` — the exact undo: the sinks back on the input net, cell and net gone with
  their routing; the released route is bound again where it is still free (true), else the sink is left unrouted.
- `int delay_cell_tap(const CellInfo *)`, `IdString delay_cell_output(int tap)`, `bool is_delay_cell(...)`.

Timing exceptions: a `-to [get_nets …]` naming the split net finds the end point by the net on its pin, which is the
cell's output net after the insertion; insert_delay_cell adds that net to such a list (remove_delay_cell takes it
out). Start points, cells and pins are not affected: the cell is neither a start nor an end point.

## Validation

- Logic, all taps: the CPE model of prjpeppercorn (`schematics/cpe/sim/cpe.v`, with `always @*`), exhaustively over
  IN1..IN8 and the six CP inputs (CLK, EN, SR, RAM_I varied with them), the input plain and inverted (`INIT_L02 =
  0011`), and for taps 0 and 1 with 32 settings of the 14 CP-line selects other nets may set: 2 129 920 checks, no
  mismatch (a control with `C_CX_I = 0` for tap 2 and `INIT_L11 = 1111` for tap 1 mismatches as it should).
- The parts of cpe.v this rests on are measured on hardware (`hw_measurements.md`): COMB2OUT of ADDF2 =
  `L10 ^ (L11 ? carry in : L02)` (S9), COUTX's routing multiplexer for C_FUNCTION 1..7 (S26), CPOUT1 / CPOUT2 through
  COUTX with an adder active (S12, S14, S28). The configurations themselves were not run on a chip.
- In nextpnr (`tests/delay_cell.cc`): the cell's model (evaluateCell) and the test simulator (with all other CPE
  inputs unknown) give IN6 for every tap; the parameters are the table above; the delays are the database's sums for
  best / typical / worst and the timing analysis takes them on the longest and the shortest path; a cell inserted
  before routing and one inserted into finished routing (route_ripped) are routed, finished, written to the
  bitstream (the words checked) and pass the logic check against the design without them; a JSON round trip keeps
  them, and routing the loaded design again works; remove after insert gives back the design and its routing.
- Not validated: the delays. Taps 0 and 1 are single arcs of the vendor database; tap 2 is put together from the
  entries the chip database gives the pips of that route (as the router times a route back through CPOUT), which may
  count the output multiplexer of OUT2 twice (`comb2_OUT2`, in the ADDF2 arc, and perhaps in `comb12_COUTX`). No
  delay was measured on hardware.
