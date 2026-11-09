# GateMate routing heuristic: a factored concept

Status (2026-11-09): implemented as `--vopt lookahead=axis` (`axis.h/.cc`), see "C++ implementation"
at the end: on dmg_soc with router and placer, TNS better on 7 of 8 placements (median -918 -> -126 ns),
Fmax +3.6%, placer 1.9x and router 2.7x slower. Measurements on the pip
graph of the empty CCGM1A1, WORST / SPEED, scripts in `docs/experiments/factored/`. All delays are pip
delays in ps (wires are 0 ps; logic arcs of the cells are not part of routing).

## The idea

Every routed connection has three parts:

1. from the source pin into the switch-box (SB) network: CPE output -> D0 of an SB (or the output mux
   OM) -> the first SB output `Yi`;
2. through the SB lattice: hops in a direction, turns, plane changes;
3. from the last SB output into the target pin: `Yj` -> input mux (IM) -> CPE input.

and the carry/propagate lines (CP lines) are a second, separate network that only runs right and up.
Parts 1 and 3 are small and the same for all CPEs of one position class; part 2 is a regular lattice.
The question was whether each part can be computed exactly and cheaply. Short answer: yes, if each part
is a small *list of states* (SB, plane, direction, cost) rather than one number, and if the distance
comes from one-dimensional run tables per SB class and direction, built on the real graph, with turns and
plane changes added as their own terms. A prototype built that way is off by 33 ps on average (99% within
150 ps) against the exact least delays, beyond 1 CPE.

## The claims, checked

### 1. Pin to network: small and cacheable, but a list, not a number

- Inside the CPE everything up to `OUT1`/`OUT2` is 0 ps of routing (`COMBOUT`, `DOUT`, `CPOUT`, `MUXOUT`
  -> `OUT*_int` -> `OUT*`), and `OUT*` -> SB `D0` is 0 ps too.
- Which SB and plane an output reaches depends on the CPE's position in its 2x2 block (die.py
  `create_sb`): CPE (i, j) drives `D0` of plane `1 + 2i + j` and `5 + 2i + j` in the two block SBs, OUT1
  at one SB and OUT2 at the other. Planes 9..12 come through the OM of each block SB (+190..223 ps), two
  of them per output and SB.
  Each 2x2 block has one big and one small SB.
- So the CPEs are equal up to their position class (x mod 4, y mod 4: block position and which block SB
  is the big one). A local search from the pin with the SB outputs as terminals takes a few hundred nodes;
  a table per class and output is enough.
- But the result matters by plane and SB: for the same target CPE, the best input pin differs from the
  worst by up to 480 ps (median per pin, sources at 6+ tiles), and OUT1 vs OUT2 differ by -160..+170 ps
  depending on the target. A single "cost to the network" loses this.
- The CP outputs (`CX/CY1/CY2/PX/PY1/PY2_OUT`) take 385..428 ps from OUT1/OUT2 (via `OUT*_IN_int`).

### 2a. Distance: costs per hop, direction dependent; "one more hop every 20 CPEs" holds only far away

The delay of a hop sits in the Y mux of the *receiving* SB and depends on which input it selects, i.e. on
the length of the wire that arrives (medians, ps; D4..D7 plus 200 ps in `SB_DRIVE`):

| input | east (`_1`) | north (`_2`) | west (`_3`) | south (`_4`) |
|---|---|---|---|---|
| D0 (start) big / small | 203 / 193 | | | |
| D2 (2 CPEs) big / small | 279 / 335 | 200 / 258 | 187 / 239 | 200 / 244 |
| D3 (4 CPEs) big / small | 328 / 300 | 236 / 217 | 237 / 212 | 243 / 225 |
| D4 (8) | 400 | 385 | 346 | 344 |
| D5 (12) | 466 | 424 | 351 | 388 |
| D6 (16) | 450 | 447 | 420 | 426 |
| D7 (20) | 560 | 473 | 438 | 469 |

- East is about 35% more expensive than the other directions (D3 chain: 75..82 ps per CPE east, 53..61
  otherwise; one D7: 38 ps per CPE east, 32..34 otherwise). Any symmetric distance term is wrong by that.
- A long hop costs about as much as two short hops, but the shortest one (D4) spans 8 CPEs = two D3
  hops: level at 8 CPEs (east 600 vs 600..656; north, west, south 544..585 vs 424..486), clearly cheaper
  from D5 on (12 CPEs: 551..666 vs 636..984; 20 CPEs: 638..760 vs 1085..1640). In exact end-to-end runs
  the gain shows from about 16 CPEs (east 1571 ps at 16, 1557 at 20, 1772 at 18), because start and
  landing are bound to the 4-grid of big SBs (see 2d).
- The straight-line cost is not monotonic in the distance (east: 17/18 CPEs 1772 ps, 19/20 1557 ps).
- Far away the lattice is periodic: 20 CPEs further west, north or south cost exactly one more D7 hop
  (+638 ps west, constant for 36..52 CPEs). East mixes D6 and D7 (+586 or +760), because their ratios are
  close (40.6 vs 38 ps per CPE).

The exact cost along one axis is a coin change over the hop lengths {2, 4, 8, 12, 16, 20} with the rule
that 8..20 start and land at big SBs; a run table per SB class and direction does it without any
fitting (RUN, below).

### 2b. Direction change: yes, a YDIAG stage plus a second Y mux

A hop arrives at `Dk_i`, which only feeds `Yi` (straight on). To turn, the signal goes `Yi -> YDIAG ->
Yj`: 113 + 238 = 351 ps at a small SB, ~200 + 249 = ~450 ps at a big one. In the lattice the measured
penalty of one turn is 350..362 ps (median), up to ~460 ps (p90) where the turn has to be at a big SB.
At a small SB the YDIAG reads `Yi_int` before the output inverter, so a signal turns where it arrives on
a hop, not at an SB it has already left on `Yi`. Turns are free at both ends: the start at `D0` goes into any `Yi`, and a CPE's IM listens to the `Y` of
the four neighbouring SBs (`IM.D0..D3` from W, S, E, N), so the last hop can come in straight.

### 2c. Plane change: yes, and expensive: about 220 ps per step on a ring of 12

`YDIAG` of plane p also feeds `X14`/`X23` of planes p+1 and p-1 in the same SB, so planes form a ring of
12. Measured in the lattice (2D targets, ring distance 1..6): 214, 437, 666, 886, 1117, 1314 ps, i.e.
additive, p10..p90 within +-40 ps. The step shares the YDIAG with a turn; on a path without a turn
(arriving in the direction it left) it needs a YDIAG of its own and costs one turn (~350 ps) more. Cheaper ways around a plane change exist only at the ends, and that is why parts 1 and 3
must return planes:

- source: two direct planes (one per block SB) and two OM planes per output and block SB;
- target: `IM.D6/D7` take the IM of plane q+-4 (planes 9..12: q+-2) and of one of the planes 9..12 of the
  same CPE (die.py `alt_plane`, ~ +560 ps for the extra IM stage), `IM.D4/D5` the IM of the diagonal
  neighbours (~ +580).

### 2d. Big SB alignment: right in effect, but along the direction, not across

- SBs sit on every CPE with x = y (mod 2) and alternate big/small with period 4, so *every* row and column
  has a big SB every 4 CPEs; there is no sideways step to a "big SB lane". Each 2x2 block has one big SB,
  reached directly from its CPEs.
- The cost is along the direction: long wires start and land on that 4-grid, a leftover of 2 CPEs costs a
  D2 hop. At short range D2 and D3 cost about the same, so the 2-CPE granularity is nearly free (east: 3/4
  CPEs 911 ps, 5/6 917 ps). At long range a target at offset 1 or 2 (mod 4) from the landing pays ~300 ps
  (east: 21/22 CPEs 1887 ps vs 23/24 1665 ps). So the cost only appears where long wires are used, as
  stated, and the axis table contains it without a separate term.

### 3. Network to pin: like 1, in reverse

Per CPE class and input (IN1..8, CLK, EN, SR) a reverse search with SB outputs as terminals gives the
entry list (SB, plane, arrival direction, cost): `Y -> IM.Dk` 206..213 ps, `IM.Dk -> IM.Y` 240..250 ps,
plus the alternative planes and diagonal neighbours above. A plain neighbour connection is 200 + 206 + 240
= ~646 ps. Within +300 ps of the best entry there are typically 4 entries per pin.

### CP lines: validated, and much stronger than expected

| step | delay (ps) |
|---|---|
| `OUT1/2` -> `CX/CY1/CY2/PX/PY1/PY2_OUT` (enter) | 428 / 404 / 405 / 408 / 403 / 385 |
| per CPE right: `CINX -> COUTX`, `PINX -> POUTX` | 116, 120 |
| per CPE up: `CINY1/CINY2/PINY1/PINY2 -> COUT/POUT` | 79, 76, 81, 73 |
| turn right -> up: `CINX -> CY*_OUT2`, `PINX -> PY*_OUT2` | 166, 170 |
| turn up -> right: `CINY2 / CINY1 -> CIY12`, `PINY* -> PIY12` | 296 / 612, 594 |
| leave: `CINX -> TI4`, `PINX -> TI8`, `PINY1 -> TI2/TI6`; `CINY2 -> CLK`, `PINY2 -> EN` | 11; 0 |

- Only LUT inputs 2, 4, 6, 8 (the `TIk` wires behind `INk`), CLK and EN are reachable, only for dx, dy >= 0,
  and every CPE passed must have its chain in pass-through.
- Within that, CP lines beat the fabric clearly: the CPE above is 414 ps (PY1) instead of 646; straight up
  they win to beyond 10 CPEs (dy = 10: 1143 ps), to the right up to 5..6 CPEs, and right-then-up as well
  (dx = 2, dy = 3: 871 ps).
- The cost is a closed formula: enter + hops x + (turn) + hops y + leave, exact on the empty chip.

## The concept: axis tables plus turn and plane terms

The distance through the lattice is a table too, but a one-dimensional one: turns and plane changes are
left out of it and added as their own terms. What makes the cost depend so strongly on which CPE of the
source 4x4 tile goes to which CPE of the target tile (at the same tile offset the least delay spreads by
~1.1 ns: 5 tiles east 1277..2444 ps, 3 east and 3 north 2154..2794 ps over the 256 CPE pairs) is carried
by the exit and entry lists (which SB, plane and direction a CPE uses) and by the SB class the runs start
from (where the big SBs are on the lane).

```
h(src, dst)  = min(h_fabric, h_cp)
h_fabric     = min over exits e = (S0, p, i, c_e) of src and entries n = (S1, q, j, c_n) of dst of
                 c_e + path(S0, i -> S1, j) + plane(p, q, turned) + c_n
path         = RUN[class(S0)][i][a]                                   if i = j and S1 = S0 + a*u_i
             = min over o of RUN[class(S0)][i][a] + TURN[class(C)][i][j][o] + RUN[class(C + o)][j][b]
                                                                      if i, j perpendicular,
               C = S0 + a*u_i, S1 = C + o + b*u_j, a, b >= 0 and even, o in {0, (1,1), (-1,-1)}
plane        = RING[ring distance of p and q] + (no turn on the path and p != q ? one turn : 0)
h_cp         = closed formula of the CP lines (dx, dy >= 0, CP-reachable target, CP routing enabled)
```

Tables, all built at start-up from the chip database for the chosen timing mode (nothing fitted):

- **RUN** per SB class (8: x mod 4, y mod 4 with x = y mod 2), direction (4) and even length: least delay
  from `Yi` of the SB to `Yi` of the SB a CPEs further on, on one plane, from a search on the real graph
  along the lane. To the chip size that is 8 x 4 x 82 = 2.6 k entries. It holds the coin change of 2a and
  the big-SB alignment of 2d, direction-dependent, without a formula.
- **TURN** per corner SB class, arriving direction, leaving direction and o: from arriving on `Yi` at the
  corner to leaving on `Yj` at the corner (o = 0: small SB 353 ps, big SB 458 ps) or at its diagonal
  neighbour (o = +-(1,1) through `X12`/`X34`: 574..678 ps). The diagonal step is needed when the row of
  the source SB and the column of the target SB have different parity (SBs only sit at x = y mod 2).
- **RING** 0, 214, 437, 666, 886, 1117, 1314 ps for ring distance 0..6; a plane change on a path without a
  turn costs one turn more, because it needs a YDIAG of its own.
- **Exit** per CPE class (x mod 4, y mod 4) and output: list of (SB offset, plane, `Yi`, cost), from a
  local search with SB outputs as terminals, pruned to best + 600 ps.
- **Entry** per CPE class and input pin: list of (SB offset, plane, `Yj`, cost), reverse search.
- **Router wires** map to lattice states: a `Yi` is itself (it can only go straight on from its SB); an SB
  input `Dk_i` arrives at its SB (a turn is possible there); a long node (`SB_DRIVE.*`, `D4..D7`) is the
  min over its landing SBs (this is where the old tables failed, see the 2026-11-08 long-wire analysis);
  `YDIAG`/`X*` are a turn state; IM and CPE wires use the entry tables (and the hybrid near tables where
  they exist).

Cost per call: folding the planes into the exit/entry pairs and keeping only directions that point to
the target's quadrant leaves 24 (p90 36) candidate pairs per (source, target pin, quadrant), each 2 RUN and
1..3 TURN lookups; for a router wire (one source state) only the ~4 entries of the sink. The candidate
lists can be precomputed per (source class, output, target class, pin, quadrant) and pruned further by
dominance.

The two-dimensional lattice table (L per SB class and start direction over all offsets and planes) was the
first prototype; it is slightly more accurate (30 vs 33 ps) but needs 0.6 M entries and hides the
structure. The axis tables are 1.6 k entries in the prototype (lengths to 40).

## Prototype accuracy

Python prototype: exits/entries from local searches, RUN/TURN and the lattice from searches on the real
graph from 8 SB classes x 4 directions (plane 1). Reference: exact Dijkstra from both outputs of 8 source
CPEs (all phases) to the 11 inputs of all CPEs within +-30 tiles; 6000 random pairs (the same for all
rows). Mean |error| / mean error (ps) by Manhattan distance:

| variant | 1 | 2-3 | 4-7 | 8-15 | 16-60 | all | within 150 ps |
|---|---|---|---|---|---|---|---|
| **axis tables + TURN + RING (`proto_axis.py 350`)** | 132/+86 | 21/0 | 25/-2 | 27/+9 | 34/+6 | **33/+6** | **99%** |
| 2D lattice, plane-exact (`proto_eval2.py`) | 36/+36 | 19/+14 | 21/+12 | 24/+14 | 31/+11 | 30/+11 | 99% |
| 2D lattice, plane-exact within 8, ring beyond (`proto_eval3.py`) | 36/+36 | 19/+14 | 21/+12 | 33/0 | 40/-2 | 38/-1 | 97% |
| 2D lattice on plane 1, planes as 220 ps per ring step (`proto_eval.py`) | 257/-256 | 107/-94 | 89/-71 | 47/-17 | 39/+1 | 42/-3 | 96% |

The axis variant needs three details to work: turns only where a hop arrives, the diagonal step for the
parity, and the extra turn for a plane change on a path without a turn. Without them it was at 222 ps.

Not good enough, for comparison: a separable table indexed by (source CPE class, target CPE class, tile
offset) per axis, `T(DX, 0) + T(0, DY) - T(0, 0)`, gives 86 ps (per pin) or 132 ps (best pin plus a plane
table); even fitted by least absolute deviation with direction-dependent end terms it stays at 45 ps
(`a9.py`, `a10.py`). The runs have to start and end at SBs, not at CPE tiles, for the sum to hold.

For orientation (other sample, `distance_model.md`, pin to pin): the model 154/142/101/90/84 (16-31)/114
(32-63); the full tables 0/0/0/2/44/136 with extrapolation. The hybrid default is exact within 4 tiles and
the model beyond, so the gain is beyond 4 tiles: ~30-35 ps instead of ~85-115 ps. Worst cases of the axis
variant: -205..+763 ps (the 1-CPE band, 7 samples, is covered by the hybrid near tables).

## C++ implementation (`--vopt lookahead=axis`)

`GateMateAxis` (`axis.h/.cc`) builds everything at start-up from the chip database (3-6 s on 32 threads):

- RUN per SB class and direction from one search along a lane on plane 1 (SB wires only), from an SB near
  the edge the run starts from, so the table spans the chip; TURN as the median, per corner class, of
  "arrive on Yi, leave on Yj" (o = 0 or a diagonal step) on those searches; RING from a search over all
  planes (SB wires only); the CP-line table from searches over CPE wires.
- Exits and entries for every wire of a sample tile per class (x mod 4, y mod 4) near the centre, from
  searches with the SB outputs and CP line nodes as terminals (3 ns limit), kept within 600 ps of the best
  in each direction (`axis-prune`), minus those another state reaches (exits) or is reached from (entries)
  at no more cost. Entries are grouped per (SB, direction) with the plane change folded in; exits per
  (SB, direction) with a delay per plane. 30 states and 24 groups per wire on average, 450 k states.
- Tables are keyed by a node's wire with the least (dx, dy, index), not by its root, so that the key does
  not depend on the position (a no-op on CCGM1A1, whose roots already are at that end).
- `estimate()`: exits in the order of a lower bound (the lower envelope of RUN per direction, one turn if
  both axes are far), entries in the order of their delay, path = straight, one turn, or a U-turn on the
  lane; exits that head away from the target first turn at the first SB that allows it (a big SB at once,
  a small one after a hop) and then go on with one more turn.
- `lookahead=axis` keeps the delay tables within `hybrid-radius` (4) tiles of a pin, where they are exact
  (also with `place-delay=table`, which builds the full tables), uses the axis estimate beyond, the model
  where it has no value (outside the core). `place-delay=axis` does the same for the placer (not tested).

Three things the prototype had not shown, found in exact paths of the worst estimates:

- U-turns: a wire heading away from its target turns back at the next big SB (Y -> YDIAG -> opposite Y,
  often with a plane step and a diagonal step at once).
- A big SB's YDIAG reads its Y outputs, so a signal can turn at the big SB it leaves (a small one only
  where a hop arrives). Without that, long wires (which land at big SBs) were ~350 ps too pessimistic.
- Bridges: a path may go through a neighbouring CPE (IM -> LUT -> OUT -> SB) to change direction and plane
  at once (~850 ps for up to 5-6 ring steps). As exits of the start wire they are kept (pruning per
  direction); in the middle of a path they are not modelled. Taking plane changes over all wires (bridges
  included) made the estimate optimistic (-90 ps on SB wires), so RING stays on SB wires; both are
  logged at start-up.

Accuracy (`--vopt lookahead=axis --vopt eval-estimate=200000`, CCGM1A1 WORST/SPEED, mean |error| / mean
error in ps; `table` = full radius-12 tables with extrapolation, `near+axis` = what `lookahead=axis` gives):

| pairs | table | model | axis | near+axis |
|---|---|---|---|---|
| wire to input pin | 89 / -42 | 125 / -9 | 77 / +49 | 75 / +44 |
| output pin to wire | 104 / -37 | 123 / +25 | 59 / +38 | 64 / +27 |
| output pin to input pin | 85 / -46 | 104 / -7 | 35 / +6 | 34 / +5 |

By the kind of the start wire (wire to input pin): CPE, IM and OM wires 32-46 / +3..+26; SB wires 98-152 /
+77..+106 (mid-path bridges, more than two turns). Time per call: table 29 ns, model 39 ns, axis 2.7 us
(down from 26 us by the grouping and the lower bounds; still the open point).

dmg_soc (Makefile flags: `--router router2 -o fpga_mode=speed -o time_mode=typical --vopt timing-fixes
--vopt place-delay=table --vopt mux-chains`, repairs off: `route-repair=0 place-repair=0`), `lookahead=table`
(the Makefile's) against `lookahead=axis`, same seed = same placement (identical TNS after placement),
domain `cc_bufg_inst_O` (33 MHz), scripts `dmg_soc_run.sh`, `dmg_soc_summary.py`:

| placement seed | table TNS ns / failing / MHz / router2 s | axis TNS ns / failing / MHz / router2 s |
|---|---|---|
| 1 | -961 / 693 / 27.23 / 37 | -865 / 632 / 27.30 / 112 |
| 2 | -1455 / 1090 / 27.18 / 41 | -668 / 642 / 27.76 / 113 |
| 3 | -610 / 777 / 27.18 / 37 | -133 / 149 / 26.83 / 112 |
| 4 | -3988 / 1587 / 26.50 / 32 | -1555 / 1293 / 27.58 / 121 |
| 5 | -990 / 821 / 26.31 / 33 | -319 / 372 / 27.29 / 137 |
| 6 | -875 / 919 / 26.91 / 29 | -510 / 591 / 27.52 / 109 |
| 7 | -113 / 92 / 29.99 / 49 | -96 / 85 / 29.54 / 167 |
| 46 | -578 / 594 / 28.53 / 33 | -780 / 803 / 28.23 / 96 |
| **median** | **-918 / 27.18 MHz / 35 s** | **-589 / 27.55 MHz / 113 s** |

TNS better on 7 of 8 placements (sign test p = 0.035), failing endpoints fewer on 7 of 8, mean TNS -1196
-> -616 ns; Fmax geometric mean +1.1% (5 of 8 better); router2 3.2x slower, iterations alike. dmg_soc's TNS
is mostly one handshake path times the ~1000 endpoints behind it, so TNS amplifies small changes there;
Fmax barely moves. A first run on seed 46 with an earlier build gave -346 ns, so routing alone moves the
TNS of one placement by a few hundred ns. One design so far.

With the placer too (`lookahead=axis place-delay=axis`; placements now differ per seed, so compare across
seeds; table = `lookahead=table place-delay=table` as above). `both` is the first build, `fixed` the build
with the corrected lower bound and the cache (below):

| seed | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 46 | median | mean |
|---|---|---|---|---|---|---|---|---|---|---|
| table TNS ns | -961 | -1455 | -610 | -3988 | -990 | -875 | -113 | -578 | -918 | -1196 |
| both TNS ns | -120 | -2206 | -1088 | -116 | -5263 | -127 | -454 | -263 | -359 | -1205 |
| **fixed TNS ns** | -224 | -106 | -162 | -65 | -68 | -96 | -147 | -166 | **-126** | **-129** |
| table MHz | 27.23 | 27.18 | 27.18 | 26.50 | 26.31 | 26.91 | 29.99 | 28.53 | 27.18 | 27.46 (geo) |
| both MHz | 29.48 | 26.03 | 27.96 | 29.43 | 25.25 | 25.82 | 26.94 | 29.07 | 27.45 | 27.45 (geo) |
| **fixed MHz** | 27.45 | 29.00 | 29.36 | 27.85 | 30.28 | 25.39 | 29.83 | 28.78 | 28.89 | **28.45 (geo)** |

failing endpoints (median): table 799, both 319, fixed 108. Run time (median, 8 runs in parallel on 32
threads): table HeAP 31 s / SA 62 s / router2 35 s; both 46 / 329 / 211 s; fixed 24 / 117 / 95 s.

The fixed build: TNS better than table on 7 of 8 placements (seed 7: -147 vs -113), all eight between -65
and -224 ns (table: -113..-3988), Fmax geometric mean +3.6% (6 of 8 better). The bimodal results of the
first build came from a wrong lower bound: it took the entries of a wire to lie within 4 tiles of it, but
2016 wires (sink pins among them) have entries up to 24 tiles away (through a neighbouring CPE), so for
those the search stopped too early and the estimate was too high. The router-only rows above (`axis`) were
made with that build too and are not repeated yet.

### Speed

`estimate()` per call: 26 us in the first version, 2.2 us after grouping the exits and entries (per SB and
direction, with the plane change folded in) and ordering the exits by an admissible lower bound (lower
envelope of RUN per direction, at most one turn counted, the entries' real reach plus one tile for a
diagonal step, the least entry delay per plane precomputed). On top, a per-thread direct-mapped cache
(2^17 entries): the estimate depends on the positions only through the classes of the two tiles and their
offset, so (class, wire, class, wire, dx, dy) is an exact key; router (the same arcs every iteration) and
placer (the same pin pairs at the same offsets) hit it most of the time. With repeats as in eval-estimate
0.37 us per call. `GATEMATE_AXIS_CHECK=1` recomputes every estimate without bounds and cache and logs any
difference (dmg_soc placement, seed 1: none).

## Open points

- Speed: still router2 2.7x and SA 1.9x slower than with the tables (a miss costs ~2 us). Candidates:
  fewer exits per wire (most come from IM/CPE wires through a CPE), a larger cache, a per-sink table of
  the lattice part near the sink.
- Repeat the router-only comparison and other designs with the fixed build.
- SB wires are estimated 80-100 ps too pessimistic (bridges in the middle of a path, three or more turns).
- More designs (multi1, corescore25, vexii), the placer (`place-delay=axis`), and the default flow
  (`lookahead=hybrid`) as a second baseline.
- The prototype is pessimistic by +6 ps on average (max +763): not admissible in the strict sense.
  Reason not yet checked; candidates are per-plane delay differences (a hop varies +-20 ps by plane, the
  run tables were built on plane 1), entries translated by 4 tiles and paths with two turns.
- The run tables were read off the lattice up to 40 CPEs; for the whole chip they come from a search along
  a lane (cheap). The far field is periodic (2a), but end-to-end accuracy beyond 30 tiles is not measured.
- The tables assume an endless lattice: near the chip edge, the IO ring and the die-to-die
  boundary (CCGM1A2+) it is optimistic. CPE bridges (routing through a CPE) and mixed fabric/CP paths are
  not in the tables; on the empty chip they are rarely the least delay.
- CP-line estimates only make sense where the router may use CP lines as routing (`use_cp_for_cpe`,
  `use_cp_for_clk`).
- Evaluated only CPE output -> CPE input within +-30 tiles, one corner; RAM/IO pins, clocks and far pairs
  still to check.
