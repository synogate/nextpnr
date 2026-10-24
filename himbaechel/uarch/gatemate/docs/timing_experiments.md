# GateMate timing experiments

What was tried in October 2026 to raise Fmax for GateMate in nextpnr, how it was measured, what
came out, and what was learned. It is written so that the comparison can be repeated once the
known timing model bugs (tests/BUGS.md: 6, 23, 28, 31 among others) are fixed. Every number below
was measured with the timing model as it was then, and the optimisations read that model, so some
results may change.

Everything here is behind `--vopt` switches (off by default). Options and code that gave no gain
are marked `TEMPORARY` in the sources; delete them after the repeat if they still give nothing.

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
- `--vopt timing-fixes` (TEMPORARY) fixes bug 31 and the other simple timing bugs (7, 23, 24, 25,
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
| `place-repair=N` | After routing: move a LUT tree (or swap two, or move a register with its LUT tree) to where the exact slack model promises most slack, lay its critical connections on their ideal paths, re-route the rest, keep if Fmax rose | 100 rounds with `route-repair=50`, against `route-repair=20`: picorv32 +10.0% (23/24, median 49.6 -> 53.4 MHz); congested +5.0 / +3.1 / +5.4% (15/15); user design +4.4% (5/5) |

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

### Testing

- Every transformation has a simulation test (random netlists compared with the reference
  netlist) and was checked by a deliberate mutation that the test must catch. Twice the mutation
  passed and showed a gap in the simulator: it treated a lower half without INIT_L20 as a
  pass-through (now unsupported once repacked), and it gave a register an undefined start value
  by name in both netlists (a register holding the inverse after `fuse-luts` is marked
  `FUSE_INVERTED`).
- Test restrictions that steer around known bugs go through `avoid(KnownBug::X)`, so fixing the
  bug and deleting its entry shows every restriction to lift.

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
