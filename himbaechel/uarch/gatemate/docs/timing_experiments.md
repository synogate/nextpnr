# GateMate timing experiments

What was tried in October 2026 to raise Fmax for GateMate in nextpnr, how it was measured, what
came out, and what was learned. It is written so that the comparison can be repeated once the
known timing model bugs (tests/BUGS.md: 6, 23, 28, 31 among others) are fixed. Every number below
was measured with the timing model as it was then, and the optimisations read that model, so some
results may change.

Everything here is behind `--vopt` switches (off by default, except `route-repair` and `place-repair`,
which now run until timing is met unless given a number of rounds; 0 turns them off; `repair-margin=P`
makes them aim P percent above each clock's constraint). Options and code that gave no gain are
marked `TEMPORARY` in the sources; delete them after the repeat if they still give nothing.

## How to measure

### Designs

- **picorv32** (`experiments/designs/`, `make_designs.sh`): N attosoc cores of
  `ice40/smoketest/attosoc` with a per-core `din` input, LEDs XORed, synthesised with
  `synth_gatemate -luttree -nomx8` (yosys 0.67). `multi1` fills 6% of a CCGM1A1 (uncongested);
  `multi8`, `multi12`, `multi15` fill 48%, 71% and 89% (congested).
- **A user design** (not in the repository): a RISC-V core with a flip-flop register file,
  about 5.7k cells (2313 DFF, 1606 MX2, 731 MX4, 438 L2T4, 234 ADDF, 4 BRAM), 15% full, with its
  own CCF and SDC. Its limit is the register file: 32 write-enable LUTs each driving about 30
  register enables, and the read multiplexers.

### Protocol

- `experiments/compare.py OUTDIR --designs ... --seeds 1-24 --variants a b --reference a -j 8`
  runs every design x variant x seed, reuses finished logs, and prints per design: routed count,
  median and range of Fmax, and against the reference the geometric mean of the per-seed ratios,
  the median ratio, wins, and a sign test. Variants are defined in its `VARIANTS` table
  (`--vopt` names, `ARG:` for other arguments, `ENV:` for environment variables); a design may
  have extra arguments in `<design>.args`.
- Compare **paired by seed**: seed-to-seed variance is large (routing alone explains about 80% of
  it, see below). 24 seeds for picorv32, 5-10 for the bigger designs.
- Use the deterministic annealing refinement. `--parallel-refine` is not deterministic (the same
  seed varies by about 5%), which ruins paired comparisons.
- `compare.py` copies the binary to `build/nextpnr-himbaechel.<outdir>` once per output directory;
  relinking while runs are going gives "Permission denied" and mixed versions. To add a variant to
  a finished directory with a newer build, copy the new binary over that snapshot only if the old
  variants' code paths did not change.
- Timeout 3600 s: router2 sometimes never resolves its last overused wires at 89% fill (also the
  unmodified binary).
- Where the time goes: read the critical path report *before* GateMate's post-route netlist step
  (the second to last report). The last report comes after the bridge/CP-line split
  (bug 6 territory) and can show a different, tying path with odd delays.

### Tools

- `critpath.py`, `depth.py`: split a critical path into logic, the least routing delay for the
  same placement on the empty chip (`offset_map.tsv`: the least delay from a CPE output to a CPE
  input by tile offset), and the excess above it; `depth.py` also takes the netlist written with
  `--write` and lists each level's type, inputs and fanout, and which neighbouring levels could
  merge.
- `fusable.py`: on a synthesised netlist, how many LUTs could fuse into all their users within a
  CPE half.
- `--vopt report-crit-routing`: for each critical-path connection, the routed delay, the table
  minimum, the muxes passed, the ideal path (A* ignoring other nets) and who holds it.
- `place_learn.py`, `learn_analysis.py`: the placement and routing learning experiments.
- `perf record -F 99` and `perf report --tid <main thread>` for where the run time goes.

## Caveat: paths the timing analysis does not see (bug 31)

LUTs fed from a CPE's internal inputs COMBIN and CINY1 have no timing arcs (tests/BUGS.md, bug 31).
So paths through a LUT that drives a register and further logic, through the fifth input of every
5-input LUT, and through adder carry-outs (comparisons) are not analysed. All Fmax numbers here are
with that bug, so they are optimistic, and an optimisation that changes which paths go through
such an arc changes what is visible:

- `mux-chains` on the user design reported -26.8% (0/10): merging a 2:1 multiplexer that fed a
  register and further logic into a 4:1 multiplexer removed its COMBIN pass-through, and a
  45 ns path from a block RAM through about 30 logic levels became visible. The design's real
  limit is probably that path (about 22 MHz), not the 35 MHz reported.
- `fuse_l2t5_inputs` moves logic into exactly such an unanalysed path (the fifth input).
- So results of rewrites that touch these structures (`mux-chains`, `fuse-luts`,
  `enable-to-lut`) say little until the bug is fixed, and the user design's ranking of what
  matters will change.
- `--vopt timing-fixes` (TEMPORARY; since then these fixes are the default and the option has no effect) fixes bug 31 and the other simple timing bugs (7, 23, 24, 25,
  26, 28, 29, 33, 37; see tests/BUGS.md) to measure with a truer model now. Variants `*_tf` in
  `compare.py`. With it (all their failing tests pass, nothing else fails):

  | Design | Default flow | Best (tables, spreading, repair 50, move 100) | Best + `mux-chains` |
  |---|---|---|---|
  | User design, 10 seeds (median) | 16.6 MHz (26.6 reported without the fixes) | 22.4 MHz, +35.1% (10/10) | 24.2 MHz, +45.2% (10/10); +7.5% over best, 10/10, p=0.002 |
  | picorv32, 24 seeds | 37.7 MHz (40.3 without) | 50.1 MHz, +34.0% (24/24) | 49.8 MHz, -1.3% over best (9/24) |

  The user design's critical paths are now 23-28 logic levels (13.5-17 ns logic) plus about
  33.5 ns routing (21.4 ns at least for the placement), from a block RAM or register to register
  data and enable inputs: logic depth matters much more than the buggy model showed.

  The rewrites again, under the fixes, against the best setup:

  | Rewrite | User design (10 seeds) | picorv32 (24 seeds) | Congested 48 / 71 / 89% (5 seeds) |
  |---|---|---|---|
  | `mux-chains` | +7.5% (10/10, p=0.002) | -1.3% (9/24) | +0.8 / +5.3 / +3.8% (3/5, 3/5, 3/4) |
  | `fuse-luts` | +0.7% (4/10) | -0.3% (11/24) | |
  | `enable-to-lut=8` | -0.8% (6/10) | -2.5% (9/24) | |
  | `mux-chains` + `fuse-luts` | +10.9% (10/10, p=0.002); +3.1% over `mux-chains` alone (8/10) | | |

  The whole flow against the default flow, under the fixes: user design +49.7% with both rewrites
  (median 16.6 -> 24.7 MHz, 10/10); picorv32 +34.0%; congested +46 / +38 / +37% (best setup) and
  +48 / +45 / +40% (with `mux-chains`). So `mux-chains` is worth keeping (on by default is a
  question for mux-heavy designs); `fuse-luts` adds a little on top of it on the user design; the
  enable pass gives nothing.

## Results

All results are geometric means of per-seed Fmax ratios against the stated reference, with wins
(seeds better) and, for 24 seeds, the sign test.

### What works

| Option | What it does | Result |
|---|---|---|
| `lookahead=table`, `place-delay=table` | Router lookahead and placer delays from shortest paths on the empty chip (per phase, anchors at the CPE pins, exact inside a CPE) instead of a linear estimate | picorv32 +8.1% (24 seeds); congested +12.9% (12/12); user design +19.5%. The table only helps the placer together with the router |
| `--placer-heap-beta=0.7` (existing) | Spreads the analytic placement | congested +15-21% on 48/71% fill; nothing on picorv32 (-4% at 0.5). Halves router time where congested |
| `route-repair=N` | After routing: take the critical connection losing most against its ideal path, free that path (rip up the nets on it and the same cell's other inputs), lay the connection on it, re-route what was ripped up with all else held, keep if Fmax rose (or tied with less near-critical slack) | picorv32 +6.2% (21/24); congested, with spreading, against tables alone +17.5 / +27.2 / +21.1%; user design about +4% over tables and spreading |
| `place-repair=N` | After routing: move a LUT tree (or swap two, or move a register with its LUT tree, or a 5-input LUT's two halves, or a 4:1 multiplexer's CPE; and, in turns with the moves, compute a critical LUT2, or a copy of it, in the first level of the LUT2 alone in a half it feeds) to where the exact slack model promises most slack, lay its critical connections on their ideal paths, re-route the rest, keep if Fmax rose | 100 rounds with `route-repair=50`, against `route-repair=20`: picorv32 +10.0% (23/24, median 49.6 -> 53.4 MHz); congested +5.0 / +3.1 / +5.4% (15/15); user design +4.4% (5/5). The pairs, multiplexers and fusing (added later): dmg_soc +0.2% (1/8, 8 seeds), little to fuse in a netlist of LUT trees |

All together on the user design: median 26.6 MHz (default flow) -> 36.1 MHz (+36%), seed range
35.3-36.3 MHz. On picorv32: 40.3 -> 53.4 MHz.

Pending when this was written: `fuse-luts` (see "Logic depth" below).

### What did not work (marked TEMPORARY)

| Option | Idea | Result |
|---|---|---|
| `sb-y-cost`, `sb-ydiag-cost` | Extra router cost for the scarce switch box Y and YDIAG outputs on non-critical nets | Inconclusive (-9% to +12%, measured with the nondeterministic refinement); 0.3 made routing fail more often |
| `lut-input-swap`, `lut-swap-rounds`, `lut-swap-moves` | Swap a LUT tree's input pairs (or a CPE's halves) to save plane changes, before routing or judged by real routing | Pre-route: noise. Routed loop: +1.5%, 18/24, but 3.6x the run time; half swaps never helped |
| `timing-opt` | nextpnr's timing_opt on unclustered LUT trees after placement | -2.8% (7/24) |
| `route-runs`, `route-learn`, `route-learn-step` | Route again from scratch with criticality learnt from earlier runs (max, ema, scale, small steps) | No learning effect: every mode within noise of plain restarts. Best of 11 restarts is +7.6% over the first, which is luck |
| `place-crit-in`, `place-crit-out`, `place-crit-mode` | The same for the placer, over full runs, raising critical and relaxing slack-rich nets in small steps | Later iterations average 3-5% below the first, on both designs |
| `place-density-*` | Pin density penalty in the annealer | Cuts its density cost 3x but more first-round overuse and lower Fmax: pin count per bin is the wrong proxy |
| `router-critical-estimate-weight` | A* weight sliding with criticality | All within noise (-1.3% to +1.3%) |
| `replicate-fanout=N` | Copy LUTs driving critical, far-spread high-fanout nets next to groups of sinks, before routing | User design +0.1%, picorv32 -2.0%: the delay moves to the copies' inputs |
| `net-repair=N` | Re-route a whole net with several near-critical sinks on their ideal paths | +0.8% over the connection repair, 8/24 |
| `--tmg-ripup` (existing) | | No effect without an unreachable frequency target; with one, +4.5% but 40x router time |
| `--parallel-refine` (existing) | | Half the run time at the same Fmax, but not deterministic |
| Serial routing (router2 without its 9-bin partition, temporary hack, reverted) | | +2.5% mean, same variance |
| Router2 bounding box margin 8, mode-0 threshold | | No significant effect |
| LUT2-only netlist (`lut2-split`, `lut2-l20`, `lut2-first-level`, `ydiag-cost`, `plane-report`, `replicate-repair`, `repair-passes`; extra LUT2 pin pairs in the chip database) - code removed | Every LUT2 alone in a CPE half, so the router picks each input's plane and spares the switch boxes' diagonal muxes; LUT2s merged again into L20 and first-level LUTs; post-route copies of drivers and repeated repairs | dmg_soc: -24.0% at first (every in-tree LUT2 level a CPE pass and a hop), -1.1% (3/8) against the original flow with the same post-route repairs at best. Plane changes and diagonal use drop, Fmax does not follow (placement-bound paths, 37% fill). Full record in test-design/OPTIMIZATION_LOG.md 6-10 |

## What we learned

### Where the time goes

- On the empty chip, a CPE output to a neighbouring CPE input takes 0.64-0.70 ns (the floor); a
  mux stage about 233 ps; further tiles 30-40 ps each.
- The linear delay estimate underestimates short connections 2-3x and overestimates long ones
  about 3x; the placer's estimate barely correlates with routed Fmax (0.08) unless the design is
  spread (then 0.5-0.7).
- picorv32 after everything: critical path about 19 ns = 5.7 ns logic + 8.5 ns least routing for
  the placement (8 hops, floor 5.1 ns) + 5.1 ns excess spread over several connections.
- User design after everything: about 28 ns = 7.5-8.6 ns logic (13-15 levels) + 14-16 ns least
  routing (14 hops, floor 9 ns) + 3-7 ns excess, mostly one register-file write enable to a
  register enable input.
- Before the repairs, much of the excess came from inputs of the same CPE competing for its input
  muxes (IN1/IN2 of a LUT2 pair) and from other nets holding the ideal path; the route repair
  addresses exactly that. After it, critical connections are within 50 ps of their ideal path.
- 71% of LUT input connections change routing plane at least once; CPE outputs reach planes 1-8
  directly only through one big and one small switch box. Swapping LUT inputs to avoid this
  changed little.
- In the final routing the small switch boxes' YDIAG outputs are 78% used.

### Variance and learning

- Routing alone explains about 80% of the seed variance: on one placement, router seeds range
  38.8-49.3 MHz.
- The critical paths are mostly the same nets in every run (Jaccard 0.65-0.73), and the per-net
  routed delays are nearly identical: the excess is structural, not luck. So learning
  criticality across runs (router or placer) only reproduces luck; targeted repairs judged by
  real routing work.
- Spreading makes the placer's estimate predictive and cuts the variance (CV 3-5% instead of
  6-11%).

### Making post-route moves work

- Re-routing with a fresh router2 call renegotiates from scratch and unravels the converged
  routing (overuse 951 -> 10k). Hold the rest: raise all WEAK wires to STRENGTH_PLACER (router2
  reserves them for their net), route, lower again.
- A move judged by a criticality-weighted sum of connection delays finds only tiny moves. The
  exact local model works: with s the slack of each connection of a LUT tree now, S the worst,
  and d each delay's change by the tables, the worst slack through it becomes
  `min over sinks (s - d) - max over inputs (d - (s - S))`; with a register, where paths end and
  start, `min over all (s - d)`.
- Moves judged that way still got worse once routed, because the moved cell's connections were
  routed through whatever the held routing left. Laying its critical connections on their ideal
  paths first (displacing what is there) is what made them pay.
- Registers can move: their clock gets a new branch from the existing clock routing with the
  clock router's rules. On picorv32 they are almost never worth it (a register splits paths, so
  what shortens the path into it lengthens the paths out); on the user design they are.

### Speed

- The repairs first used router2 for each attempt: setting it up walks every wire of the chip
  (more than half of the run time), and when nothing fits it searches the whole chip before
  failing (about a minute each).
- Re-routing the few ripped connections directly (A* over free wires within 4, then 16 tiles of
  their bounding box, longest first, retried with the failing one first) finds everything router2
  found: 15 minutes -> 30 seconds per picorv32 run, identical results.
- One timing analysis per round: the analysis after a kept round, or before an undone one,
  describes the next round's starting state.
- Compiler flags are not the lever (the build is already -O3). `-ffast-math` would break the
  infinity sentinels and determinism.

### Logic depth

- At the LUT level, synthesis leaves little: only about 2% of the LUTs could fuse into their
  users within a CPE half (abc already maps to that structure).
- picorv32's critical paths contain inverters (`CC_LUT1`): 21 of 24 final critical paths, a
  quarter of their levels. They feed 2:1 multiplexers, registers' D inputs, 4:1 multiplexers and
  adder B inputs.
- Much of the space goes into CPE halves the packer fills with pass-throughs: the upper half of a
  5-input LUT (only its fifth input), registers whose D cannot merge with its driver (20% of the
  user design's halves), a second half to pass out a LUT that also feeds a register, and block RAM
  inputs.
- `fuse-luts` (being measured): before packing, fold inverters into the LUTs and 2:1 multiplexers
  they feed, into the LUT feeding them, or behind the register they feed (flipping its set/reset
  and initial values); fuse a LUT into at most two users when each still fits a half; after
  packing, let the upper half of a 5-input LUT compute the LUT feeding its fifth input. It
  removes 151 cells of picorv32 and 127 of the user design.
  - Fusing a LUT into many users is bad: one with 40 users made each of its inputs 40 sinks
    bigger and the routing much slower (limit: two users).
  - A 2:1 multiplexer only fits a CPE half with its select on both first-level LUTs.
  - First measurement (before the two-user limit and the upper-half fold): picorv32 +2.5%
    (16/24); user design -2.9% with a much wider spread (30.7-38.8 MHz).
  - With both (on top of `route-repair=50 place-repair=100` and spreading): user design +4.3%
    (7/10 seeds, median 35.1 -> 36.1 MHz, worst seed 32.3 -> 34.1, best 39.4; not significant);
    picorv32 -0.1% (14/24); congested picorv32 -2.9 / -3.8 / -3.7% (5/15 better) although the
    placer's estimate rose on all three, so the fused netlists route worse there. Design-dependent;
    off by default.
- Copying a register's D driver into the register's own pass-through half has no target on the
  final critical paths: every path ending at a D input already ends in a LUT in the register's
  CPE. The other endpoints are register enables (5 of 8 on the user design) and block RAM
  address inputs (most congested picorv32 runs).
- `enable-to-lut=N`: where a register's half would only pass its data through,
  that LUT takes the enable instead (D = EN ? D : Q, enable tied high), so a write enable reaches
  LUT inputs on any plane rather than the enable plane. Applies to 1541 registers of the user
  design (the register file's 1056 among them) and 55 of picorv32. Measured globally (N = 8):
  user design -10.5% (0/10), picorv32 -6.8% (3/24); the placer's estimate drops as much, and
  the extra feedback connections add 18% wires on the user design.
- `mux-chains`: two 2:1 multiplexers in a chain (the first feeding only the
  second's data) are one 4:1 multiplexer, always (the outer select as S1, the other data on both
  inputs of the other side). 402 merges in the user design, one such pair on each of its final
  critical paths; 18 in picorv32, none critical. Measured globally: user design -26.8% (0/10),
  but that is a hidden path becoming visible (bug 31, above); picorv32 -1.9% (9/24).
- Rewriting only where it matters (the global passes cost routing on the congested designs):
  - `rewrite-depth=F`: only cells on paths at least F times as deep, in logic levels, as the
    deepest. Depth is a poor stand-in for timing criticality: picorv32's deepest paths
    (14 levels) are not its critical ones (9 levels after the ROM), so almost nothing gets
    rewritten; the user design's deepest paths count 27.5 levels against 13-15 on its critical
    ones.
  - `rewrite-crit-in=file`, `rewrite-crit-min=F`: only cells driving a net that
    was at least F critical after routing in an earlier run (`place-crit-out`). Two runs per
    result; `compare.py` variants `*_co` (writes the file, same result as without) and
    `*crit_rew*` (`{crit_of:VARIANT}` takes the file of that variant's run with the same seed).
    picorv32 seed 1 at 0.9: 150 cells, 4 inverters folded, 3 LUTs fused, 33 enables moved.
    All four rewrites at 0.9: user design +3.7% (7/10, median 35.1 -> 36.2 MHz, range
    31.6-39.6; not significant), picorv32 -2.5% (9/24).
- `cpe-merge=N` (under `timing-fixes`; without them its merged paths are exactly
  the unanalysed ones): two LUT levels into one CPE. Where B, fed by A, depends on its other inputs
  only through one function of them (B = G(A, V(rest))), A goes into the CPE's upper half, V into
  the lower half and G into L20: one LUT delay and one routing hop less. B becomes a CC_L2T5 with A
  on its fifth input, and `fuse_l2t5_inputs` moves A into the upper half after packing. N = 1:
  only where A feeds nothing else (no extra space); N = 2: also with a copy of A (cells marked by
  `rewrite-crit-in` if given). On the user design's 10 critical paths (under the fixes), 32
  neighbouring pairs fit without a copy and 26 with one; 24 do not split so. Globally N = 1
  merges 127 pairs in the user design and 132 in picorv32; N = 2 525 and 353 (408 and 275 copies).
  Measured, N = 1: user design +0.8% on top of `mux-chains` + `fuse-luts` (6/10; 72 merges left
  after those), picorv32 +0.7% (13/24). Not significant. On the user design it did shorten the
  critical paths (median logic 14.2 -> 10.9 ns, levels mostly 20-24 -> 18-20), but routing on them
  grew (28.5 -> 30.8 ns): a merged CPE takes up to 8 inputs from two cells' sources, which is
  harder to place close to and crowds its input muxes. Less depth only pays where the routing
  does not grow back.
  That was with placers that did not see the paths through multiplexers and adders (bugs 40, 41). With
  them fixed, N = 2 on dmg_soc: Fmax +8.5%, all 16 seeds better ("The logic levels of the loop" below).
  Over the 18 benchmark designs N = 2 gives +13% and shorter runs, and is the default since
  (`cpe-merge=0` turns it off; "The two options on the benchmark designs" below).
- Further depth rewrites, not done, with what the data says:
  - 8:1 multiplexers: yosys has `CC_MX8`, the GateMate packer does not, hence `-nomx8`.
  - Two neighbouring LUT trees whose combined function fits a whole CPE (three LUT2 levels over
    up to 8 inputs): several pairs on every critical path of the user design (`depth.py`, an
    upper bound until checked exactly).
  - Fusing with duplication (a LUT with more users) only where a path is critical, after routing.
  - Shannon decomposition on the latest-arriving input of a critical cone.
  - Inverters into adders and 4:1 multiplexers (needs the packed CPE's tables).
  - Retiming across logic (register moves that change cycle boundaries): the largest, and the
    hardest to keep exact with set/reset and initial values.

### 8:1 multiplexers (CC_MX8)

- A CC_MX8 takes a whole CPE (`CPE_MX8`): data on IN1..IN8, selects on CLK, EN and SR through their
  LUT1s (S0 and S1 also over CINY2/PINY2), neither register usable. Its data inputs have nothing to
  undo an inversion on the way (unlike LUT inputs, the MX4's C bits or the register controls), and
  router2 knows nothing of polarity: about half of them arrive inverted.
- After routing, `fix_mx8_inputs()` routes the inputs that arrive inverted again, with a search over
  wire and polarity (displacing other nets where needed). What remains is an error.
- dmg_soc with yosys' MX8 (272 of them, seed 46): 1096 of 2176 data inputs arrive inverted, too
  many to route again, so the design fails; Fmax was also lower than the 28-31 MHz without MX8.
  Synthesis keeps `-nomx8`; MX8 is for single places, where the repair succeeds.
- Not on main (branch `mx8-net-polarity`): inverting nets instead (LUT trees by their last LUT,
  registers storing the inverse with set/reset swapped, an MX8 output; their other sinks undo it).
  On dmg_soc it left 630 instead of 1096 to route again, still 62 failing in the end. Routing the
  inputs before router2 made other connections unroutable.
- A parity-aware router2 would solve it properly but touches the common router.
- The MX4 alternative (two 4:1 multiplexers per CPE, the second masking with the third select)
  depends on how the second multiplexer of a CPE shares the C and X bits, which the schematic and
  nextpnr disagree on and no hardware test has settled.

### Testing

- Every transformation has a simulation test (random netlists compared with the reference
  netlist) and was checked by a deliberate mutation that the test must catch. Twice the mutation
  passed and showed a gap in the simulator: it treated a lower half without INIT_L20 as a
  pass-through (now unsupported once repacked), and it gave a register an undefined start value
  by name in both netlists (a register holding the inverse after `fuse-luts` is marked
  `FUSE_INVERTED`).
- Test restrictions that steer around known bugs go through `avoid(KnownBug::X)`, so fixing the
  bug and deleting its entry shows every restriction to lift.

## November 2026: the paths the placers did not see (bugs 40, 41)

Looking for what limits dmg_soc with the repairs off (`route-repair=0 place-repair=0`, Makefile flags,
`lookahead` and `place-delay` at their default `axis`, 16 placement seeds 1-15 and 46, 33 MHz clock;
all numbers below are from these runs, in parallel on one machine, so the times are under load).

- **Where it stood:** Fmax 25.2 to 29.8 MHz (geometric mean 28.0), median TNS -442 ns, while the timing
  printed after placement was 28.8 to 35.4 MHz. The worst path has 23 to 26 connections over 180 to 370
  tiles, 21 to 25 ns of routing for 10 to 12 ns of logic. It is the loop from the cache RAMs through the
  bus arbiter and router to the CPU's ready, the next fetch address and back to the RAMs' address inputs
  (or the enables of about 1100 registers), and it passes the data inputs of two 4:1 multiplexers.
- **Cause:** the placers never saw that loop as one path. Until `repack_cpe()` merges them after
  placement, the halves of a multiplexer or adder CPE are two cells without a net between them, and
  the carry out of an adder half had no timing class (tests/BUGS.md 40). Paths through a multiplexer's
  data inputs or along a carry were several short paths to the placers, placed with no regard for each
  other: the arbiter 50 to 70 tiles from the router it feeds in the bad seeds, next to it by luck in the
  good ones. That is most of the seed spread, and why the placement estimate said little about the
  routed result. The registers of the CPU's counters and the register file's write data (300 to 380
  endpoints) had paths of under 1 ns to the placers and 20 to 33 ns routed.
- **Fix:** timing links between the halves while the design is placed, the carry arcs, and no routing
  delay for the carry to the CPE above (bug 41: about 0.5 ns per CPE before, 8 ns on a 32 bit adder,
  which showed once the chains were in the analysis and made the counters look critical).

  | | Fmax (geometric mean, range) | TNS median / mean | Failing endpoints (median) | Wires |
  |---|---|---|---|---|
  | before | 28.04 MHz, 25.17 - 29.84 | -442 / -543 ns | 387 | 509k |
  | with the paths | 29.95 MHz, 24.18 - 34.18 | -91 / -754 ns | 71 | 502k |

  Fmax +6.0%, 12 of 15 seeds better, TNS better on 11; two seeds meet 33 MHz without repairs (none did
  before). One seed (9) is missing: its router run kept one wire overused for 1800 iterations and was
  stopped (the stall known from other placements). And one seed (12) is far worse than any before, 24.2 MHz
  and TNS -9356 ns: there the placers see the loop and still leave it at 26 MHz (TNS -4241 ns after
  placement). So the fix shows them the path; it does not make them good at shortening it.

  Three more sets of 16 seeds with the same links differ from the code in the tree in one detail each
  (the rounding of the router's criticality weight; with the RAM pin estimate of the table below; with an
  arc from the second bit's operands to the first bit's sum in two-bit adders, which is wrong and made
  loops in the tests): Fmax +9.5% (15 of 16), +7.6% (13), +9.5% (16), median TNS -69, -69 and -34 ns, one
  seed with TNS below -1000 ns among the 48 (two among the 16 before). Over all four sets Fmax is +8.2%
  (56 of 63 runs better). Each detail changes every placement, so the sets are four samples of the same
  thing, and the spread between them (6.0 to 9.5%) is what 16 seeds resolve.

  Placement took 29% longer (the refinement has more to do) and routing 9% less (medians of runs side by
  side on a loaded machine; not measured idle).
  Everything measured before on designs with 4:1 multiplexers or adders (which is all of them: the user
  design has 731 MX4 and 234 ADDF without `mux-chains`) was placed without these paths, so the ranking
  of the placement options in the tables above (the placer learning, `timing-opt`, the heap weights, how
  much `place-repair` brings) is open again.
- **Half RAMs in the refinement:** the annealer picks a new bel near a cell's place, or anywhere for
  types with fewer bels than `placer1/minBelsForGridPick` (64): the 32 RAM sites, but not their 64
  halves, so half RAMs never moved after the analytic placement. GateMate now sets one more than the
  number of half RAMs (65 on a CCGM1A1). In the
  bad seeds the cache RAMs sat in the outer RAM columns with their logic 55 to 90 tiles away, and the
  refinement then swaps 20 to 70 RAMs; alone (before the fix above, 8 seeds) Fmax +0.1%, TNS better on
  6; with the fix against the fix without it, 8 seeds: Fmax +1.3% (3 better), mean TNS -79 against
  -190 ns. Not significant either way; kept because a RAM stuck far from its logic is what the bad
  seeds had in common.

- **Critical arcs keep their wires (router2, `router2/criticalPresentShare`, GateMate 0.2):** an arc that
  is more critical than every other net on a wire pays less of the wire's present congestion, so the nets
  with slack give way first. Before, which net left an overused wire depended on whose detour was cheaper,
  and the worst path lost 0.3 to 3.8 ns to wires held by other nets. An arc of criticality c pays
  1 - 0.8 c^4 of the present congestion. On the placements of the fix (same placement, routed with and
  without, 15 seeds to compare):

  | | Fmax (geometric mean) | TNS median | Failing endpoints (median) |
  |---|---|---|---|
  | the fix | 29.95 MHz | -91 ns | 71 |
  | with the priority | 30.76 MHz | -56 ns | 37 |

  Fmax +2.7%, better on 15 of 15, TNS on 12. Seed 9, whose router run stalled, routes in 53 iterations.
  Against the state before the fix, all 16 seeds: 28.04 -> 30.61 MHz (+9.2%, 14 better), median TNS
  -442 -> -58 ns. Seed 12 stays where it was (24.3 MHz): the router cannot mend that placement. There the
  data cache's RAMs are 64 columns apart, five cells of the loop sit with the far ones, and the loop
  crosses the chip six times.

  With the same share of 0.2 for all arcs the gain is the same (+2.7%, 15 of 15) but the router takes a
  third longer: the conflicts among nets far from critical resolve more slowly (iterations 3 to 6 route
  1.5 to 2 times as many nets, each slower). Graded by c^4 it takes no longer than without the priority
  (three seeds side by side: 1749 s against 2120 s without and 2777 s with the flat share). Graded by c^2:
  +2.3% (13 of 15). A flat share of 0.5: +2.2% (14 of 15). A flat share of 0 (a critical arc never pays
  for a less critical net): iteration 5 to 9 after 45 minutes, stopped.

- **Directed moves in the refinement (`placer1/directedMoveCrit`, GateMate 0.5):** what the outlier seeds
  had in common is cells of the critical loop left far from the rest of it by the analytic placement, among
  cells they share uncritical nets with (seed 12 above). The refinement's random moves reach one to three
  tiles and are taken only if each step pays, which a step of a few tiles towards a path 60 columns away
  hardly does against the wirelength it costs. Now every cell outside a cluster and every cluster with a
  connection of criticality 0.5 or more also tries, once in each iteration, a bel where its connections pull
  it: the mean of the cells at their other ends, weighted like the timing cost (criticality to the 8th), within
  one tile of it; a block RAM the RAM site nearest to it. The move is taken if the cost falls, like any other.
  About 2000 to 2600 such moves per run, most in the first iterations. 16 seeds against the state with the
  router priority:

  | | Fmax (geometric mean, range) | TNS median / mean | Failing endpoints (median) |
  |---|---|---|---|
  | before | 30.61 MHz, 24.28 - 34.85 | -58 / -656 ns | 50 |
  | with directed moves | 32.03 MHz, 30.78 - 34.39 | -11 / -19 ns | 21 |

  Fmax +4.7% (13 of 16 better), and no seed is left behind: the worst is 30.8 MHz with TNS -87 ns, seed 12
  goes from 24.3 to 32.5 MHz. Three seeds meet 33 MHz, seven are within 2 ns of TNS. The other settings tried
  give the same picture (16 seeds each): criticality 0.8 31.75 MHz, any criticality 31.67 MHz, radius 3 with
  six tries 31.99 MHz, all with the worst seed at 29.7 MHz or better and a mean TNS of -13 to -22 ns. The refinement stops as before (no new least wirelength in an iteration), after 18 iterations in the
  median; without that, a few dozen directed moves keep coming in every iteration with nothing gained.

  From the state before this section, all 16 seeds: 28.04 -> 32.03 MHz (+14.2%, all better), median TNS
  -442 -> -11 ns, failing endpoints 387 -> 21.

- **The estimate against the routing (bugs 42, 43).** With the placers seeing the paths, what they take for
  the delays matters. Measured on the state above with the constant MX2 fold of the same day (16 seeds:
  31.68 MHz, 29.39 - 32.91, median TNS -5.6 ns, no seed at 33 MHz), over the connections on the paths into
  the 400 worst endpoints of each seed: the estimate after placement is right on average and off by up to
  2.5 ns of the period for a seed, and the worst path of a seed is routed 2.4 ns above its estimate, nearly
  all of it on connections to a CPE one to three tiles away: estimated at the 0.3 ns of a CP line and routed
  at 0.9 to 1 ns through the switch boxes. Three causes, three changes:

  - *The clock skew.* A global clock reaches the registers after 0.8 to 2.4 ns (0.95 ns in the top rows,
    2.25 ns in the middle of the chip) and the clock inputs of the block RAMs with no delay. The analysis of
    the routed design counts that, the placers' did not: for them the worst endpoint was a register behind a
    RAM in all 16 seeds, on a path with 1.4 to 2.3 ns more than its delay says, while 14 of the routed
    designs failed first at a RAM's address input. Now the placers analyse with the clock skew (`placer/clockSkew`, on for
    GateMate), from the estimates of the clock delays; these were 2.2 ns for the clock inputs of half the RAM
    halves, which the search of the clock network does not reach (bug 43). 16 seeds: Fmax +1.5% (11 of 15,
    one seed in the router's one-wire stall), median TNS -5.6 -> -2.7 ns.
  - *Inputs that cannot trade places.* The estimates come from the empty chip, where each first-level LUT
    input is reached from both pairs of CPE inputs through the swap of the subtrees. Only a plain LUT tree
    has that swap. The select S1 of a 4:1 multiplexer comes in on IN7 / IN8 or on the CP line from the left,
    S0 on IN5 / IN6 or on the one from below: S1 from the CPE below was estimated at 0.3 ns and routed at
    0.9 ns (bug 42). The worst path of a seed has three selects (the multiplexers of the critical loop),
    estimated 1.1 ns too fast in all. Now the input of a cell without the swap is estimated to the place it
    has (`estimate_sink`). The selects on the worst paths go from 1.01 to 0.93 ns each (0.74 ns with the clock
    skew too), 16 seeds alone: Fmax +0.7% (9 of 16), median TNS -5.6 -> -2.7 ns.
  - *Critical inputs of LUT trees that gave way (router2).* Where a plain LUT tree has its most critical input
    from the CPE below or left and that input sits in the other subtree, the subtrees must be swapped, and with
    them the other three inputs, which have their fastest ways as the tree stands. The swap is one select for
    the four nets, a resource in conflict while they disagree. The present cost of that conflict grew for the
    critical input like for the others (scaled by its crit weight, but with three other uses against one), and
    after seven iterations it was more than the 0.65 ns of the detour: an input of criticality 0.97 left the
    CP line to three of 0.15 to 0.3 that had not found their new places yet (seed 2, traced). The share of the
    present congestion that a most critical arc pays on a wire (`router2/criticalPresentShare`) now holds for
    a resource too. In the trace the input keeps the line and the others are in place two iterations later;
    such connections on the paths into the worst endpoints of seed 2: 19 -> 11. Same placements, 16 seeds:
    Fmax +0.6% (12 of 16), median TNS -5.6 -> -4.4 ns. What is left are trees whose other inputs are as
    critical (within 0.03 in 35 of 43 cases looked at): two inputs cannot both have the line.

  All three, 16 seeds:

  | | Fmax (geometric mean, range) | TNS median / mean | Failing endpoints (median) | Seeds at 33 MHz |
  |---|---|---|---|---|
  | before | 31.68 MHz, 29.39 - 32.91 | -5.6 / -16.6 ns | 16 | 0 |
  | clock skew and estimate | 32.26 MHz, 30.14 - 34.64 | -1.0 / -10.9 ns | 6 | 3 |
  | and the router | 32.41 MHz, 30.60 - 34.56 | -1.1 / -9.7 ns | 4 | 4 |

  Fmax +2.3% (10 of 16 better), which is at the edge of what two sets of 16 seeds differ by; the failing
  endpoints and the TNS fall in every one of the sets. Three more seeds miss 33 MHz by 0.2 ns of TNS or less.
  The router's part has no test of its own: in a small design the other inputs find their places before the
  critical one gives way, with or without it.

- **The logic levels of the loop, and the refinement (2026-11-11).** With the estimates right, the worst path
  of a seed has 25 levels: 10.9 ns of logic and 19.8 ns of routing, 14.3 ns of that on the 12.6 connections of
  four tiles and more (a connection to the next tile takes 0.47 ns, one over 2 to 3 tiles 0.60 ns, over 4 to
  8 tiles 0.80 ns, over 17 to 32 tiles 1.43 ns: every level costs a connection, wherever its cell is). The
  path runs over 224 tiles between a start and an end 61 tiles apart. About 390 of the 400 worst endpoints of a seed
  start at one block RAM, the tags of the data cache; the worst path ends at the address inputs of the
  instruction cache's RAMs in 11 of 16 seeds. Each change against the state above (16 seeds, 32.41 MHz,
  30.60 - 34.56, 4 seeds at 33 MHz):

  - *Two LUT levels in one CPE (`cpe-merge=2`, see "Logic depth").* The option was there, measured with the
    placers that did not see the paths (+0.8%, with the routing growing back what the logic saved). Now:
    835 pairs merged, 528 of them with a copy of the first LUT, 4% more CPE halves, 1% more wire. 35.17 MHz
    (33.98 - 36.29), **+8.5%, all 16 seeds better and all at 33 MHz**. `cpe-merge=1` (no copies, 316 pairs)
    and `fuse-luts` give nothing in the estimate after placement (+1.0%, 9 of 16; -0.2%, 7 of 16).
  - *The LUT in front of a RAM pin (on by default, `no-pin-lut-copies`).* A RAM input is fed through the LUT
    half of a CPE at the pin. The LUT driving the input moved into that half only where it fed nothing else;
    otherwise the half passed its result on, a level of logic (0.4 ns) and a connection more. 1724 such
    halves in dmg_soc, 660 behind a LUT (775 behind a register, 200 behind a multiplexer or an adder, which
    stay): the address of the instruction cache comes from a LUT feeding three RAM halves and one other cell.
    Now the pin gets a copy of the LUT (`GateMatePacker::move_ram_io`, for every block fed through CPEs).
    33.25 MHz (31.74 - 34.61), +2.6% (13 of 16), mean TNS -9.7 -> -1.3 ns, 10 seeds at 33 MHz, 3% more wire.
    With `cpe-merge=2`: 35.33 MHz (30.65 - 38.26), +0.5% over the merge alone (noise): a LUT merged with the
    one before it is a 5-input LUT, which does not go to a pin.
  - *A refinement that anneals (`refine-temp=3e-5`, setting `placer1/refineTemp`, off by default).* The
    refinement of the analytic placement takes only moves that improve it, and after two or three iterations
    its moves reach no further than the next tile. Started at 3e-5 it cools as the annealer does: 84
    iterations instead of 20 (the refinement takes about four times as long), timing cost 985 -> 921,
    wirelength 161000 -> 152000 (seed 2). Estimate after placement +5.9% (15 of 16) alone; from 1e-4:
    +4.6% with 107 iterations, from 1e-5: +2.2% with 57. Routed, with the two above: 36.54 MHz
    (32.87 - 39.42), +3.4% over them (13 of 16).

  | | Fmax (geometric mean, range) | Against the state above | Seeds at 33 MHz |
  |---|---|---|---|
  | the state above | 32.41 MHz, 30.60 - 34.56 | | 4 |
  | LUT copies at the pins (default) | 33.25 MHz, 31.74 - 34.61 | +2.6% (13 of 16) | 10 |
  | `cpe-merge=2` alone | 35.17 MHz, 33.98 - 36.29 | +8.5% (16 of 16) | 16 |
  | copies and `cpe-merge=2` | 35.33 MHz, 30.65 - 38.26 | +9.0% (14 of 16) | 15 |
  | and `refine-temp=3e-5` | 36.54 MHz, 32.87 - 39.42 | +12.7% (15 of 16) | 15 |

  The seed below 33 MHz in the last two rows (seed 3) fails on another path: from a FIFO's RAM to the
  HyperBus outputs, half a period (15.15 ns), 8 levels with four connections of 2 ns. With the loop faster
  this path limits a seed now and then; nothing was done about it.

  Tried and dropped (estimates after placement, 16 seeds): the three RAM halves of the instruction cache
  around the tag RAM of the data cache by LOC parameters (routed: 31.52 MHz, -2.8%, 3 of 16 better: the
  loop also runs through the CPU, and the placers' own choice is no worse); a copy of a LUT for the upper
  half of a 5-input LUT where the LUT feeds other cells too (+0.4%, 8 of 16; 16 such LUTs); for
  `cpe-merge`, the pairs on the deepest paths first and of a LUT's inputs the deepest (+0.6% over the plain
  order, 9 of 16), and a split of the pair where the upper half takes inputs of the second LUT too (109
  pairs more, -1.1%, 7 of 16).

  *The two options on the benchmark designs.* `cpe-merge=2` and `refine-temp=3e-5` on the 18 designs of
  `docs/experiments/designs`, 10 seeds, without the repair passes; seeds 1 - 5 with the Windows build (30
  runs at a time, 90 minutes limit), 6 - 10 with a static Linux build on a server (40 at a time, 4 hours),
  the three variants of a seed on one machine (the two builds give different results for a seed). Fmax is the
  geometric mean over the seeds, the gain the mean of the ratios by seed, the run time the median ratio by
  seed (536 of 540 runs; four of seed 10 were still running):

  | Design | CPE halves used | Fmax without | `cpe-merge=2` | run time | `refine-temp=3e-5` on top | run time |
  |---|---|---|---|---|---|---|
  | multi1 | 6% | 49.94 MHz | +4.5% (8 of 10) | x0.87 | +3.4% (7 of 10) | x1.11 |
  | picorv32 | 17% | 34.40 MHz | +10.0% (9 of 9) | x0.88 | +2.1% (7 of 9) | x1.32 |
  | lm32 | 20% | 29.49 MHz | +5.5% (9 of 10) | x0.78 | +3.4% (8 of 10) | x1.31 |
  | mandelbrot | 17% | 40.96 MHz | -0.7% (4 of 10) | x0.89 | +1.1% (8 of 10) | x1.73 |
  | micropython | 15% | 22.35 MHz | +7.2% (9 of 9) | x0.87 | +2.0% (8 of 10) | x1.21 |
  | minerva | 26% | 22.83 MHz | +1.5% (7 of 10) | x0.78 | +3.1% (9 of 10) | x1.43 |
  | mor1kx | 25% | 18.58 MHz | +33.2% (10 of 10) | x0.81 | +3.1% (7 of 10) | x1.27 |
  | neorv32 | 21% | 27.99 MHz | -0.1% (4 of 10) | x0.87 | -1.0% (5 of 10) | x1.37 |
  | vexriscv | 21% | 33.34 MHz | +6.7% (8 of 10) | x0.93 | +5.6% (10 of 10) | x1.17 |
  | vexii | 32% | 31.95 MHz | +7.8% (10 of 10) | x0.82 | +4.3% (9 of 10) | x1.58 |
  | corescore | 17% | 37.63 MHz | +18.9% (10 of 10) | x0.84 | -0.0% (6 of 10) | x1.45 |
  | corescore25 | 42% | 32.17 MHz | +11.1% (10 of 10) | x1.07 | +4.9% (9 of 10) | x2.12 |
  | vexsmp2 | 68% | 15.53 MHz | +56.4% (9 of 9) | x0.85 | +4.8% (7 of 9) | x1.47 |
  | coreblocks | 82% | 12.90 MHz | +37.3% (9 of 9) | x0.74 | +5.2% (5 of 5) | x1.83 |
  | corescore45 | 75% | 27.13 MHz | +11.2% (9 of 9) | x0.96 | +0.6% (5 of 10) | x2.84 |
  | multi8 | 47% | 40.64 MHz | +14.1% (8 of 8) | x0.70 | +5.2% (6 of 8) | x1.62 |
  | multi12 | 71% | 38.84 MHz | +14.9% (9 of 9) | x0.70 | +8.3% (7 of 8) | x2.07 |
  | multi15 | 90% | 39.19 MHz | +17.2% (10 of 10) | x0.74 | +1.6% (3 of 5) | x2.08 |
  | all runs | | | **+13.2% (152 of 172)** | x0.84 | +3.1% (126 of 164) | x1.44 |

  - `cpe-merge=2` is the **default** since (`cpe-merge=0` turns it off, 1 merges without copies): no design
    loses more than noise (mandelbrot, neorv32), the full ones gain the most, and the runs get shorter,
    since the router has fewer nets and converges sooner. Runs that did not end: 6 without it (5 where
    router2 ran into the time limit on a last few overused wires), 1 with it. A LUT is only merged with one of
    its own region (a floorplan region would lose it to the other LUT's CPE otherwise), and the upper half
    is named `<second LUT>$upper`.
  - `refine-temp=3e-5` stays an **option**: +3.1% on top for 1.1 to 2.8 times the run time; on the full
    designs the run takes twice as long and more (of the first five seeds, 9 of the 25 runs on coreblocks,
    vexsmp2, multi8, multi12 and multi15 ran into the 90 minutes, 7 of them in the router's first
    iterations; with 4 hours none ran into the limit).
  - Four runs ended with router2's `Failed to route arc` (multi12 without options, multi8 with the merge,
    multi15 and coreblocks with both; in iteration 1 to 3, both builds): an arc that finds no path even
    without its bounding box. Not looked into; it is neither option's.

What was tried on the way and gave nothing (8 seeds unless noted, against the runs before the fix;
the environment switches they were behind are not in the tree). The builds with these switches computed
the router's criticality weight with a `float` exponent, which rounds differently: the same placements
route a little differently than with the code in the tree, so the comparisons below are among such
builds.

| Idea | Result |
|---|---|
| Criticality relative to each clock pair's constraint instead of its longest path (the 12 MHz clocks and the unconstrained clock crossings count as critical as the 33 MHz clock) | Fmax +1.4% (5 better), TNS mixed: noise. The half-period paths into the HyperBus PHY (12 endpoints, 15.15 ns) then get less weight and limit more seeds |
| Placers ignore nets of more than 200 users (the BUFG experiment's effect without a BUFG) | Fmax +0.5% (3 of 7), TNS better on 2 |
| Annealer timing weight 0.8 instead of 0.5 | Fmax -2.5% (2 better), more wire; one seed took the router 42905 iterations |
| Router criticality weight `1 - crit^8` or `1 - crit` instead of `1 - crit^2` | -1.8% and -1.2% |
| Placing again around the RAMs of a first placement (RAMs locked, the logic back to the analytic placer) | First 8 seeds +4.7% (6 better), all 16: -0.2%, mean TNS -1721 against -408 ns: where the second placement scattered the loop, nothing held it together. With the fix above it adds nothing either (29.89 against 29.95 MHz, 8 seeds), for 60% more placement time |
| Estimating the nets between a RAM and the CPEs at its pins as they are (0 to 0.15 ns routed, about 1 ns estimated) | With the fix, 16 seeds: Fmax -1.7% (6 better), one seed with TNS -2721 ns. The estimate being too high seems to help: these paths lose most in routing |
| With the fix, 16 seeds against the fix: annealer timing weight 0.7 instead of 0.5 | Fmax -1.3% (7 of 15 better) |
| With the fix, 16 seeds: analytic placer's timing weight 30 or 100 instead of 10, or 30 with criticality exponent 4 instead of 2 | +1.9% (9 of 15), +1.1% (9 of 15), -2.9% (5 of 15): no trend, and the first and the last have a seed of their own with TNS below -2900 ns |

Where the rest goes, with all of the above (worst path of each seed, mean of 16): 19.8 ns of routing
against 18.0 ns estimated at placement. Of the 1.7 ns, 1.3 ns are on connections to a CPE one to three
tiles away: 31 of 156 such connections are routed 0.3 ns or more above the estimate, all into plain LUT
trees, where another input of the tree as critical, or another net out of the source CPE, has the CP line
or the place the line comes in at. The estimate gives every input its fastest way; which of them can
have it together it does not know. The longer connections are 0.4 ns above their estimates in all, wires
held by other nets.

## Repeating the comparison after the timing fixes

1. Rebuild; the delay tables are computed at start-up from the chip database, so they follow any
   delay changes.
2. Regenerate the designs if yosys changed (`experiments/designs/make_designs.sh`).
3. Run, for example:
   - `compare.py runsA --designs multi1.json --seeds 1-24 --variants base both rep20 rep50_prep100 fuse2_rep50_prep100 --reference base`
   - `compare.py runsB --designs multi8.json multi12.json multi15.json --seeds 1-5 --variants both b07 b07_rep20 all_rep50_prep100 all_fuse2_rep50_prep100 --reference both`
   - the user design with its `.args` (CCF, SDC) and the `all_*` variants.
4. Repeat the TEMPORARY experiments that were close (`lut-swap-rounds`, `router-critical-estimate-weight`,
   critexp 4 / heap timing weight 30) before deleting them; the learning experiments only if the
   seed variance changes a lot.
5. Recheck where the time goes (`depth.py`, `report-crit-routing`): the untimed paths of bugs 23,
   28 and 31 (5-input LUT COMBIN, carry arcs) may become critical, which changes what to work on.
