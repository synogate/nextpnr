# GateMate distance model

## Results

Default since 2026-10-30: `hybrid` with radius 4 for both the router (`lookahead`) and the placer
(`place-delay`): the delay tables within 4 tiles, the closed-form model beyond (see Hybrid below).
`linear`, `table`, `model` and `hybrid` stay selectable, each option on its own (a lone
`--vopt lookahead=table` leaves the placer at hybrid).

dmg_soc, primary clock `cc_bufg_inst_O`, seeds 1-8 (`experiments/distance/runs_hybrid2/summary.md`,
`runs_base3/summary.md`; hyb6 from `runs_hybrid`, model4 from `runs_model4`). Ratio = per-seed Fmax over
table3 (the full radius-12 tables with extrapolation), geometric mean, wins-losses. Build time on the idle
32-thread machine, table memory:

variant | median Fmax MHz | ratio vs table3 | W-L | table build | tables
---|---|---|---|---|---
linear3 | 26.34 | 0.904 | 0-8 | - | -
model4 | 27.22 | 0.975 | 3-5 | - (ms) | -
hyb2 | 28.44 | 1.001 | 2-6 | 0.2 s | 3.6 MB
**hyb4 (default)** | **29.38** | **1.014** | 5-3 | **0.3 s** | **11.6 MB**
hyb6 | 29.26 | 1.022 | 5-3 | 0.4 s | 24.1 MB
hyb12 | 29.50 | 1.031 | 5-3 | 1.0 s | 89.2 MB
table3 | 28.77 | 1 | - | 3.9 s (10.8 under load) | 89 MB

Pooled over multi1 (12 seeds), dmg_soc (8), corescore25 and vexii (5 each), timeouts excluded: hyb4 0.978 of
table3 (11-18) and 1.058 of model4 (24-5); hyb12 1.005 of table3 (17-12) and 1.094 of model4 (25-4).

Recommendation (taken): hybrid radius 4 as the default: 0.3 s and 11.6 MB, level with the full tables on
dmg_soc, about 2% below them pooled (4% on multi1). `--vopt hybrid-radius=12` (1 s idle, 89 MB) is level
with the full tables everywhere; worth it where memory and a second of start-up do not matter.

`--vopt lookahead=model` (router, `estimateDelay`) and `--vopt place-delay=model` (placer,
`predictDelay`) estimate the least routing delay between two wires on the empty chip with a closed
formula (`distance.h/.cc`). It replaces the 89 MB delay tables (`table`, about 10 s to build) with a
per-tile-type classification of the wires by name (milliseconds at start-up) and about 75 constants.
`linear` (100 + 100 ps per tile, the default before hybrid) and `table` remain for comparison. The die-to-die
penalty (+2000 ps) and the clock tree case of `predictDelay` are unchanged.

## The formula

Each wire is classified once per tile type by its name (prjpeppercorn `die.py` naming). The class
says where the signal is, a switch box (SB) or CPE at an offset from the wire's tile (nodes are named
after their lexicographically first wire, so a segment may be named after its far end), on which
plane, and whether it already travels in a direction:

| State | Wires | Position |
|---|---|---|
| fresh | `YDIAG`, `X12/X34/X14/X23`, `D0`, `OM.*`, unknown wires | the SB (X endpoints: the YDIAG they belong to, also on the neighbouring plane) |
| short, direction i | `Yi`, `Yi_int`, `D2_i`/`D3_i` (and `_CLK`, `_D2D`), `IM.D0..3_n` | the SB driving the segment (inputs: the SB itself) |
| long, direction i | `SB_DRIVE.*.OUT`, `.IN`, `D4_i..D7_i_n` | the big SB driving the segment |
| input mux in/out | `IM.Pp.D*`, `IM.Pp.Y` | the CPE tile |
| bridge | `CPE.IN*`, other `CPE.*` | the CPE tile, routed on through the CPE |
| CPE output | `CPE.OUT1/2`, `SB.D0_n`, `OM.D*_n` (output nodes named after a mux input) | the CPE that drives it |

For a target CPE input of plane q (IN1..8 = planes 1..8, CLK/EN/SR = 9..11) at offset (ax, ay)
from the position, with a = along, c = across the direction for travelling states:

```
cost = base(wire) + plane_step * planes(p, q)
     + 21 * (d1 + d2) + 88 * (seg(d1) + seg(d2)) + 73 * (d1 % 2 + d2 % 2) - 16 * max(0, 12 - d1 - d2)
     + 60 * (left(d1) + left(d2))
     + turn(m) + (ax > 0 ? 82 + 4 * ax : 0)          [+ 486 if a fresh state sits on the target tile]
seg(d)  = 3 * (d / 20) + min((d % 20) / 4, 3)        segments: one per 4 tiles up to 12, a long one to 20
left(d) = 6 <= d < 20 and d % 4 >= 2                 2 or 3 tiles left over after a 4-tile segment
turn(m) = 0, 94 or 224 for m = 0, 1, >= 2
fresh:      d1 = |ax|, d2 = |ay|, m = min(d1, d2)
travelling: d1 = |a|, d2 = m = c
  short: a >= 1: + credit;  a = -1 or 0 (level with the SB): - 245
  long:  a >= 7: + credit, d1 = max(a - 20, 0);  a = 3..6 (short of the first landing): - 300
```

`planes(p, q)` is a 13x13 table built at init: half steps of a breadth-first search over the planes,
one step per neighbouring plane at an SB (YDIAG -> X14/X23, a ring of 12), 2.5 steps for arriving
on one of the two alternative planes the input mux takes (D6/D7, `alt_plane` in die.py), capped at
3 steps; `plane_step` = 214 ps.

A CPE output starts at the two SBs of its 2x2 block: on the one plane whose D0 it drives in each
(die.py `create_sb`), or through the output mux (+89 ps) on two of the planes 9..12 (`create_outmux`);
the estimate is the least of these with `base` = 1053 ps (a table of 2 outputs x 4 block positions x
2 SBs built at init). Input mux wires take their direct ways to the target (same tile and plane
143/0 ps, alternative plane 816/673 ps, diagonal neighbour 726/583 ps for IM.D/IM.Y) or else go
through the CPE (+617/366 ps, +537 more for the input muxes of planes 9..12, whose way through the
CPE goes through CLK/EN/SR; CPE inputs and `CPE.INk_int` +367, other CPE wires +49, then as a CPE
output with either output). Clock network wires: 3000 ps. A target that is not a CPE input but a
wire inside a CPE (a bel pin) takes the least over the CPE inputs before it plus the delay inside the CPE (from a
per-tile-type Dijkstra over the CPE's own pips, as the tables do); src and dst in the same CPE use
that delay directly. Any other target is estimated in the reverse direction from a CPE output (below), else without the plane term.

Constant terms `base` and `credit` per state (ps):

| State | base | credit (toward the target) |
|---|---|---|
| Y output of a small SB / of a big SB / `Yi_int` | 1383 / 1251 / 1126 | -398 / -398 / -192 |
| SB input D2/D3 of a small / big SB, D4..D7 (big SBs only) | 1418 / 1448, 1630 | -278 |
| SB input from the other die (`_D2D`) / clock network (`_CLK`, small / big SB) | 1425 / 1425, 1680 (+232 for `_CLK_n`) | -259 |
| long segment (`SB_DRIVE.OUT`, `D4..7_n`) / `SB_DRIVE.IN` | 1968 / 2077 | -522 / -533 |
| YDIAG small / big / `YDIAG_int` | 1015 / 1080 / 1026 | |
| X input / D0 / OM.Y / OM.D / other | 1256 / 1027 / 1030 / 1228 / 1103 | |

### From a CPE output to any wire (the reverse direction)

router2's backward search asks `estimateDelay(source pin, any wire)`. For a CPE output (or an output
node named after an SB D0 or OM input) as source and a destination that is not a CPE wire,
`reach()` estimates the delay of getting *onto* the wire: each wire also carries a reverse
classification (`classify_reverse`), where it is entered and how:

| Reach | Wires | Entered at |
|---|---|---|
| travel, direction i | `Yi`, `Yi_int` | its switch box, leaving in direction i |
| travel, direction i | SB input `Dk_i` (and `_n`, `_CLK`, `_D2D`) | the switch box 2..20 tiles back that drives the segment |
| travel, direction i | `SB_DRIVE.*.IN/OUT` | its big switch box |
| travel, direction k+1 | `IM.Pp.D0..3` | the switch box whose Y(k+1) drives it (1 tile back, 2 from a switch box tile) |
| input | `IM.Pp.D4/D5`, `D6/D7`, `IM.Pp.Y` | the input mux it comes from: the diagonal neighbour's, another plane's (ALT0/ALT1), its own |
| fresh | `YDIAG`, `X12/X34/X14/X23` (the YDIAG driving it), `D0`, `OM.*`, other | the switch box (plane of the driving YDIAG for X) |

The source starts, as in the forward direction, at the two switch boxes of its 2x2 block on the
plane it drives directly or (+`r_om`) on the two output mux planes. With (ax, ay) from that switch
box to where the wire is entered, along/across the wire's direction for travelling wires:

```
cost = r_base(wire) + travel(d1, d2, turn, ax) + r_planes(start plane, plane)
travel = the tile terms of the forward formula (21 * (d1 + d2) + 88 * seg ... + turn(m) + east), same constants
fresh:  d1 = |ax|, d2 = |ay|, m = min(d1, d2);  + r_same if at the start switch box itself
travel: d1 = |along|, d2 = m = across;  + r_credit if along >= 1 (the source behind the switch box),
        + r_level if along = 0 or -1
input:  r_base + the forward estimate from the CPE output to a CPE input of that input mux's plane and tile
r_planes = 0 / 206 / 396 ps for 0 / 1 / 2 planes along the ring (+214 per further plane, never the least)
```

| Wire | r_base | r_credit | r_level | r_same |
|---|---|---|---|---|
| Y / SB input D2, D3 of a small SB (and `Yi_int`) | 1053 | -260 | -387 | |
| Y / SB input D2, D3 of a big SB | 1078 | -352 | -388 | |
| SB inputs D4..D7, `SB_DRIVE` | 1269 | -384 | -396 | |
| `IM.Pp.D0..3` | 1277 | -311 | -396 | |
| `IM.Pp.D4/D5`, `D5_n`, `D6/D7`, `IM.Pp.Y` (input) | 306, 16, 287, -60 | | | |
| YDIAG, X | 809 | | | -237 |
| D0, OM (CPE output nodes, through another CPE) / other | 1275 / 850 | | | -145 |

`r_om` = 93 ps. Fitted like the forward constants (L1, the start plane and switch box choice redone
each iteration) on one random half of the 318 000 output-pin-to-non-CPE-wire pairs of the sample
(108.6 ps), checked on the other half (108.8 ps); the C++ reproduces the Python fit exactly on a
fresh dump. Refitting the tile terms for this direction too gained 3 ps and was left out. Destinations
inside a CPE keep the forward way (the CPE inputs before them plus the delay inside the CPE).

A wire in a CPE with no CPE input before it (behind the carry/propagate chain inputs `CINX`,
`CINY1/2`, `PINX`, `PINY1/2`) is estimated through those chain inputs: the CPE's tile at plane 0
plus `chain_in` (636 ps, the median over the sample: entering the chain input means routing to the
neighbouring CPE and through it) plus the delay inside the CPE. Before, these were 490-1050 ps
optimistic (870 mean |error|); now 267 / +116 ps (13 800 pairs).

### Where the constants come from

All constants were fitted (least absolute error, iteratively reweighted least squares, with the
choice among a CPE output's start planes and the input mux shortcuts redone each iteration) to the
exact least delays of 150 000 random pairs (wire to CPE input, CPE output to CPE input, see below)
for the worst-case corner in speed mode, then refined on 500 000 pairs (see Accuracy). The structure was derived from die.py: the turn needs a
YDIAG stage, plane changes go around the YDIAG/X ring, segment lengths 2/4 and 8..20.

At start-up, `init()` measures the mean delay of a big SB's Y mux from its D2 inputs plus the delay
into those inputs (218 ps for worst/speed) and scales every constant by measured / 218, so the model
follows `time_mode` and `fpga_mode` (scale 0.56 for best/speed, 0.72 typical/speed, 1.79
worst/lowpower).

## Accuracy

`--vopt eval-estimate=N` (`evaluate_estimates` in distance.cc) runs Dijkstra over the whole chip
to each of the 11 inputs and from both outputs of 16 CPEs near the centre (all positions modulo 4),
samples wires uniformly over the tiles' wires at offsets up to 12 (40%), 40 (35%) and 80 tiles
(25%), and compares the estimates with these exact delays. `eval-dump=FILE` writes the samples,
`eval-offset-map=docs/experiments/offset_map.tsv` adds that comparison. CCGM1A1, worst/speed,
multi1.json, 400 000 wire-to-input pairs and 100 000 pin-to-pin pairs (the sample is deterministic),
mean |error| / mean signed error in ps:

| Manhattan distance | 0-1 | 2-3 | 4-7 | 8-15 | 16-31 | 32-63 | 64+ | all | within 150 ps |
|---|---|---|---|---|---|---|---|---|---|
| wire to input, linear | 1500/-1500 | 1467/-1467 | 1406/-1406 | 1212/-1212 | 756/-708 | 972/+924 | 3597/+3597 | 1462/+301 | 4.4% |
| wire to input, table | 0/0 | 0/0 | 1/0 | 4/-1 | 55/0 | 140/-48 | 216/-171 | 89/-42 | 77.5% |
| wire to input, model (first fit) | 208/+11 | 202/-24 | 188/-10 | 179/-8 | 175/-55 | 183/-53 | 189/-30 | 182/-35 | 53.1% |
| wire to input, **model** | 176/+26 | 154/+3 | 130/+17 | 117/+27 | 111/-13 | 131/-34 | 143/-20 | 125/-8 | 67.6% |
| pin to pin, linear | 1033/-1033 | 1005/-1005 | 904/-904 | 694/-693 | 373/-206 | 1444/+1444 | 4044/+4044 | 1399/+770 | 4.3% |
| pin to pin, table | 0/0 | 0/0 | 0/0 | 2/-1 | 44/+3 | 136/-55 | 223/-187 | 85/-46 | 78.1% |
| pin to pin, model (first fit) | 154/+7 | 151/-78 | 106/-21 | 98/+18 | 97/-9 | 114/-18 | 130/+4 | 109/-4 | 72.8% |
| pin to pin, **model** | 154/+31 | 142/-52 | 101/-9 | 90/+27 | 84/+3 | 114/-33 | 133/-21 | 104/-7 | 75.3% |
| pin to wire, linear | 1075/-1074 | 1046/-1046 | 961/-959 | 753/-747 | 444/-233 | 1407/+1403 | 4101/+4101 | 1448/+775 | 6.4% |
| pin to wire, table | 41/+41 | 49/+49 | 48/+48 | 41/+38 | 68/+29 | 142/-30 | 243/-104 | 109/-6 | 75.3% |
| pin to wire, model (forward formula to the tile) | 366/-135 | 400/-262 | 356/-225 | 323/-174 | 318/-184 | 330/-216 | 330/-146 | 328/-188 | 31.6% |
| pin to wire, **model** (reverse direction) | 200/+100 | 168/+55 | 144/+64 | 123/+65 | 102/+19 | 115/-16 | 150/+30 | 123/+26 | 71.9% |

(400 000 output-pin-to-wire pairs from the same searches, destinations drawn uniformly over the
tiles' wires; the table knows 148 442 of them.) By destination kind (model, mean |error| / mean):
SB inputs 105/+10, Y 103/+3, X 113/+10, YDIAG 117/+18, `SB_DRIVE` 100/-5 (`.IN` 198/+194), input
mux wires 103-106/+5..+7, OM 127-131, SB D0 184/+70, CPE inputs 103/-11, other CPE wires 190/+105
(chain wires, see above), other 407.

Pearson r: wire to input 0.930 (linear), 0.991 (table), 0.990 (model); pin to pin 0.975, 0.992,
0.993. The table leaves out 6577 of the wire-to-input pairs (it knows nothing of them or calls them
unreachable). Beyond its radius the table extrapolates linearly and underestimates by up to 190 ps;
the model stays slightly optimistic there (-13..-34 ps at 16+ tiles, as wanted, so the router is
drawn to the long segments; linear overestimates by up to 4 ns). Short range (0-15 tiles) is
slightly pessimistic (+3..+31), which only affects the last few tiles of a route.

The first fit was refined on a dump of this sample (fitted on one random half, checked on the other:
the same numbers within 1 ps): the input muxes of planes 9..12 were underestimated by 450 ps and those
of planes 1..8 overestimated by 90 (one bridge constant for both, now `ctrl_bridge`); `CPE.INk_int`
bridges like a CPE input; the D2/D3 and D4..D7 inputs of a big SB and the clock inputs got their
own terms; short states level with their SB and long states 3..6 tiles ahead were 220-290 ps too
pessimistic; 2 or 3 tiles left over after a 4-tile segment were 50-110 ps too optimistic (`left`).
A free refit of all other constants on top of these gains under 1 ps.

By source kind (wire to input, model, mean |error|): CPE outputs 107, OM 113-127, SB D0 121,
YDIAG 121, X inputs 124, Y outputs 139, SB inputs 142, SB_DRIVE 146-189 (-22..-44: the landing big
SBs relative to the target in both axes), CPE inputs 104, other CPE wires 109, input mux wires
104-107, clock network 714 (underestimated), unknown wires 373.

The scaling held in another corner (first fit, not rerun): best/lowpower (scale 0.744, 40 000 pairs) gives wire to input
148/+5 ps (64+ tiles: 156/-97), pin to pin 88/+23 (64+: 106/-73), table 62 and 59.


Against `offset_map.tsv` (least delay over the output/input pairs from the CPE at (86, 68), 20 480
offsets): linear 3619/+3607, table 230/+60, model 307/+265 (64+ tiles: 327/+272, table 241/+12).
The table itself, exact within its radius, lies 220-290 ps above the map at short range, so the map
measures a slightly different quantity (presumably a different source CPE or more pins); against
the exact per-pair delays above, the model's long-range error is about zero.

Time per call (1e6 calls over the wire-to-input pairs, one thread, first fit on an idle machine): table
132 ns, model 42 ns, linear 4.5 ns (refined model on a loaded machine: table 222, model 51; with the
reverse direction, loaded: table 132, model 49). Most of the model's time goes to input mux and CPE wires, which evaluate both block
switch boxes for both outputs.

The post-route repairs (`route-repair`, `place-repair`, both on by default, and `net-repair`),
`lut-input-swap`, `lut-swap-rounds` and `report-crit-routing` take their least delays from
`GateMateImpl::least_delay()`: the tables when they were built, else the model. The tables are built
for `table` and `hybrid` (the default; radius 4 then) or `eval-estimate`; with `model` or `linear` for
both the repairs use the model and the start-up saves the table build. The model always gives a value, where the tables give -1 for
pairs with no CPE pin at either end; the callers that skipped such pairs now include them.

## Hybrid

`--vopt lookahead=hybrid` / `--vopt place-delay=hybrid` (the default) with `--vopt hybrid-radius=N` (default 4): the delay
tables are built with radius N (`GateMateLookahead::build` with `near_only`, the key includes the radius and
the mode) and used only within it (`lookup` returns -1 when the wire's tile is more than N tiles from the
anchor CPE in either axis, instead of extrapolating); there and for pairs with no CPE pin at either end the
model answers. The router, the placer and `least_delay()` (repairs, LUT swap) follow the same rule; d2d and
the GLBOUT clock-tree path in delay.cc are unchanged. Motivation: the model's error is largest at 0-4 tiles
(150-265 ps mean |err|, 30% wrong orderings of close alternatives at <= 4 tiles, tables 0%).

`GateMateLookahead::build(..., near_only)` skips the whole-chip search for the extrapolation slopes in this
mode (hybrid does not extrapolate); that one job took 3.1 s and was the whole 3.3 s floor of the build. What is
left are the 8 phases x 13 anchors radius-limited searches (each over radius + 8 tiles): radius 2/4/6/12 build
in 0.2/0.3/0.4/1.0 s on the idle 32-thread machine (5.5/8/12/28 CPU-s), with tables of 3.6/11.6/24.1/89.2 MB.
The values within the radius are unchanged (10k sampled `lookup()`s identical before and after, table and
hybrid mode); `table` mode still runs the slope search.

Edge effect: compared with the radius-12 tables, the radius-4 tables overestimate 0.22-0.25% of the pairs in
rings 0-3 and 0.63% in ring 4, by 1.2 ps on average there: negligible, no trust radius needed. About 3% of the
wires at every ring, ring 0 included (e.g. `SB_DRIVE.Pxx.D3/D4.IN`), reach the pin only through a detour beyond
radius + 8 tiles and are "unreachable" (20 ns) in a small table but 1-3.5 ns in the radius-12 one.
`--vopt hybrid-missing=model` sends such wires to the model; it helped multi1 (+3%) but not dmg_soc (-1.4%,
1-7), so it is off by default.

Benchmark (`experiments/distance/runs_hybrid/summary.md` and `runs_hybrid2/summary.md`, 2026-10-30): radius
2-6 are alike on every design (multi1 0.956-0.959 of table3 = radius 12 with extrapolation); radius 12 with
the model beyond closes the multi1 gap (hyb12: 1.005 of table3, 1.051 over hyb4, 10-2, p 0.039; hyb8 0.977), so
the gap was the radius, not the far-field model. Over multi1, dmg_soc (8 seeds), corescore25 and vexii (5 each):
hyb12 1.005 of table3 (17-12) and 1.094 of model4 (25-4); hyb4 0.978 of table3 (11-18) and 1.058 of model4
(24-5); hyb12/hyb4 1.030 (20-8, p 0.036). On dmg_soc alone: hyb12 1.031, hyb4 1.014, hyb2 1.001 of table3.
Same load, dmg_soc seed 1, one run at a time: table 947 s wall (build 3.9 s, HeAP 25, SA 55, router2 22),
hyb4 253 s (0.3, 23, 51, 23), model 221 s (-, 23, 39, 29); the wall gap is the post-route repair passes (3 rounds
for table, 2 for the others), one seed only. Two vexii runs (hyb4 seed 5, hyb12 seed 2) hit the 3600 s timeout
(router not converging), none of the reference variants did.

Radius: 12 if 89 MB of tables are acceptable (1 s build, level with the full tables); 4 (11.6 MB, 0.3 s) costs
about 2% Fmax (4% on multi1). Default: hybrid with radius 4 (see Results).
If one of the options is `table` and the other `hybrid` (e.g. `--vopt lookahead=table` alone), the full
tables (radius 12, extrapolated) are built and the hybrid side uses them everywhere, with the model only where
they have no value.

## Tried and dropped

- Per-axis free-form costs (one value per distance 0..40) instead of `seg()` and the tile slope:
  130 instead of 138 ps on fresh states; not worth a table.
- A 12x12 plane table fitted freely: 134 against 138 ps for the breadth-first search with 2.5 steps
  for the alternative planes; the search keeps it derived from the architecture.
- Extra terms for the target's position in its block, its plane, the sign of dy, the target tile
  holding an SB, turn and plane change sharing a YDIAG: each under 2 ps.
- Big/small SB per state helped (159 -> 151 ps on SB states) and was kept for Y, SB inputs and YDIAG.
- Long segments: a free-form profile along the direction did not improve on "the first 20 tiles
  cost the credit" (MAE 235 vs 232); their remaining error (about 200 ps) comes from where the
  landing big SBs lie relative to the target in both axes.
- The input mux wires kept a 235 ps error with one bridge constant; most of it was the planes 9..12
  (`ctrl_bridge`, now 105 ps). The rest depends on which of three CPEs (own, diagonal neighbours)
  they bridge through and with which output.

## Open issues

Diagnosis of the first seed-paired benchmarks (`experiments/distance/runs_model*`), 2026-10-30.

**1. Placer time.** Not reproduced. The same binary, `table` and `model` run at the same time on
dmg_soc seed 1 (placement only): HeAP 43.3 s (table) vs 39.1 s (model), SA 122 vs 102 s, HeAP
iterations 28 vs 23; multi1: HeAP 7.4 vs 8.1, SA 9.3 vs 9.8. predictDelay calls (counted with a
temporary counter): dmg_soc 69.4 M (table) vs 61.0 M (model); in situ 365 ns per table call, 214
per model call (both including getBelPinWire and the timer). The benchmark gap is load and binary:
spreading, which calls no delay estimate, varies 1.1-6.0 s across the eight model runs, and `base`
was built from another branch (`base2`, linear from this branch, takes 44-52 s HeAP too). A real
cost found on the way: the GLBOUT path of predictDelay (clock_tree_delays) takes 3.4-4.0 µs per
call (16-21 µs in multi1, threads contending): 4.9 M calls, 16.7 s of the 39 s spent in
predictDelay on dmg_soc, with either estimate. It builds a `stringf` key and takes the global mutex
on every call. **Fixed:** `clock_tree_delays()` now first looks the source up in up to 32 slots
per GateMateImpl (`clock_tree_slots`, a key published with release/acquire after the pointer to
the process-wide cache's entry, which std::map keeps valid), lock-free; the key string and the
global mutex (`clock_tree_search()`) are taken once per source. Measured in isolation (one
thread, uncontended): 380 ns per call before, 2 ns after; with threads contending the old path took
more (16-21 µs per call in multi1, above). Same placement and routing with `lookahead=table
place-delay=table` (multi1 seed 1: 37.34 MHz both; dmg_soc seed 1: 40.44 / 32.44 / 27.00 MHz both);
the wall times of those runs (taken under load, 5 processes) did not show a difference beyond noise.

**2. Secondary clocks.** Not the estimate. Both secondary clocks of dmg_soc are constrained to
12 MHz (huge slack, criticality about 0), so their Fmax is incidental. Same-branch linear (`base2`)
gives 29.3 / 29.6 MHz, below the model's 34.8 / 30.5; the 40 MHz of `base` is the other branch.
The critical paths of dmgpp0.clk12m have the same 59 logic stages but 15.2 ns of logic with the
model (15.5 base2) against 11.9 ns in `base`; on mandelbrot's bitmap_addr_instance.clk (88 stages,
routing 3.8 vs 4.0 ns) logic is 29.3 vs 20.0 ns: this branch gives the CP-line cells delay
(`*_cplines.POUTY1` 0.00 -> 0.37 ns, `COUTX` 0.00 -> 0.40 ns, 21 of them in the path). Routing
on dmg_clock: table 7.2, model 8.8, base2 11.4 ns. Pin kinds (`GATEMATE_EVAL_PINS=3` with
`eval-estimate`: forward Dijkstra from 3 output pins per bel pin kind to all input pins within 40
tiles, multi1, speed), mean err / |err| in ps, model vs what predictDelay used with tables
(table, else linear):
- CPE_RAMIO.I (RAM/IO data in): model +125/146, table+linear +145/254; CPE_RAMIO.OUT: -151/238
  vs -37/166; IOSEL.IN -> CPE inputs: +111/161 vs +497/647. RAM and IO are fine.
- Chain ports through general routing are ~500 ps optimistic: CPE out -> CINX/CINY/PINX/PINY
  -504..-624 (tables -67..-116), COUTX/COUTY/POUTX/POUTY/COMPOUT -> anything -117..-889 (linear
  -276..+235). These wires classify as CPE_INT with no CPE input before them, so `estimate` routes
  to the tile without the input mux and the CPE in front. Fix: for a chain input as destination,
  add targets for the CPE that drives it (its input planes plus its in -> COUT/POUT delay and the
  hop), and for a chain output as source start from the receiving CPE (bridge), or fit one
  constant per direction (`chain_in`, `chain_out`). Low priority: chains are mostly fixed clusters.
  **Destination side done** (`chain_in`, see "From a CPE output to any wire"): 870 -> 267 ps mean
  |error| on the chain wires as destinations of the pin-to-wire sample (the remaining spread
  depends on which neighbour drives the chain). With `GATEMATE_EVAL_PINS=3` (pin to pin, as
  predictDelay sees it): any output -> CINX/CINY/PINX/PINY -608..-497 / 563..637 before,
  +27..+158 / 224..279 after (tables + linear +20..+31 / 325..355). The source side is still open:
  COUTX/COUTY/POUTX/POUTY/COMPOUT -> anything -642..-234 / 547..746 before, -406..+147 / 566..659
  after (they bridge through their own CPE in the model, but go to the neighbour's chain input).
- Clock inputs (IOSEL.CLOCK, RAM.CLOCK, GLBOUT/PLL inputs) +1400..+3000; the clock router and the
  clock-tree path of predictDelay handle those.

**3. Router stalls.** The estimate, not router2 at high fill. Same placement (place-delay=model,
corescore25 seeds 2 and 5, which stall in `runs_model3`): `lookahead=model` sticks at 2 overused
wires (> 200 iterations, killed), `lookahead=table` routes in 104 and 61 iterations. The overused
wires, listed with a temporary dump in router2, are always a bridge: `IM.Pp.D5_n` (the diagonal
neighbour's input mux output) and `CPE.TIk_int` of that neighbour, fought over by two nets with
full-chip boxes and fail counts over 100. router2's backward search asks `estimateDelay(source pin,
any wire)`, for which the model is poor (329 ps mean |error|, -187 mean, see Accuracy; the tables
had exact from-pin values there), and the search stops where both directions first meet, whatever
the congestion cost. With `GATEMATE_MODEL_PIN_DST_ONLY=1` (kept in delay.cc: model only toward a CPE
input, linear otherwise) both placements route: 107 and 91 iterations, 23.7 / 23.2 MHz (table 24.6
/ 24.3, one seed each). Fix: make that the behaviour of `lookahead=model`, or better give the model
a proper to-wire direction (for a destination that is not a CPE input, estimate to the CPE inputs
it can reach, `targets` in reverse, rather than to its tile at plane 0), then rerun the benchmark.
**Fixed** with the reverse direction ("From a CPE output to any wire": pin to wire 328/-188 ->
123/+26 ps); the env var is gone. Same placements (post-placement Fmax 30.40 / 29.12 MHz as
before), `lookahead=model place-delay=model`: seed 2 routes in 47 iterations, 24.04 MHz, router2
77 s; seed 5 in 95 iterations, 21.92 MHz, router2 102 s (machine loaded, 5 processes). Seed 5's
Fmax is below table (24.3) and pin-dst-only (23.2); one seed each, so this is not yet a verdict on
quality, only that the stall is gone. Smoke tests: multi1 seed 1 34.08 MHz (table/table 37.34),
router2 28 s, wall 67 s; dmg_soc seed 1 dmg_clock 35.49, clk12m 30.67, cc_bufg 22.82 MHz (table/table
40.44 / 32.44 / 27.00), 81 iterations, router2 64 s, wall 658 s. Both below the tables on one seed:
rerun the seed-paired benchmark.
