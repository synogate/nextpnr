# Hybrid delay estimate (`nextpnr-himbaechel.distance_hybrid.exe`): exact tables near, closed-form model far

hybN = `--vopt lookahead=hybrid --vopt place-delay=hybrid --vopt hybrid-radius=N`: delay tables built with radius N
and used without extrapolation, i.e. only where the anchor CPE and the other wire's tile are within N tiles in
both axes (pin-related pairs); everything else (beyond N, or no CPE pin at either end) goes to the closed-form
model. Router, placer and the post-route repairs/LUT swap (`least_delay()`) all follow that rule. References:
table3/linear3 from `runs_base3`, model4 from `runs_model4` (other binaries, other dates, other machine load).
Fmax: final report; dmg_soc primary clock `cc_bufg_inst_O`, others the highest-constraint clock. Ratio =
per-seed Fmax a/b, geometric mean; ties apart, two-sided sign test on wins/losses.

Load: Stage A (multi1) ran next to another 8-process benchmark (`runs_split`) on a 32-thread machine; Stage B
ran mostly alone. Wall times are therefore not comparable across directories (the references ran under
their own, different load); the hyb6 wall-time and router2-time advantage in Stage B (e.g. dmg_soc 472 vs 702 s wall, 40 vs 78 s router2) may
be largely load; it needs a same-load rerun before it counts.

## Table build per radius

`--pack-only` runs on multi1, idle machine, two repetitions each (identical within 0.1 s). Size = bytes of
`values`. The build time has a floor of about 3.3 s that does not depend on the radius: the whole-chip search
for the beyond-radius slopes and the per-tile-type hop/inside preparation, which hybrid mode does not need for
the slopes (they are unused) but `build()` was reused unchanged.

radius | values bytes | MB | build s (idle) | build s (under load, in the benchmark logs)
---|---|---|---|---
2 | 3,567,200 | 3.6 | 3.3 | 7.1
3 | 6,991,712 | 7.0 | 3.3 | 7.1
4 | 11,557,728 | 11.6 | 3.4 | 7.1
6 | 24,114,272 | 24.1 | 3.5 | 7.2
12 (table3 default) | 89,180,000 | 89.2 | 3.9 | 8.1

## Stage A: radius sweep, multi1 seeds 1-12

#### Per variant

design | variant | Fmax median MHz | finished/runs | timeouts | median wall s | median router2 s
---|---|---|---|---|---|---
multi1 | hyb2 | 38.99 | 12/12 | 0 | 41 | 14
multi1 | hyb3 | 37.72 | 12/12 | 0 | 41 | 14
multi1 | hyb4 | 39.27 | 12/12 | 0 | 41 | 14
multi1 | hyb6 | 39.16 | 12/12 | 0 | 41 | 14
multi1 | table3 | 40.48 | 12/12 | 0 | 37 | 11
multi1 | linear3 | 32.98 | 12/12 | 0 | 44 | 23
multi1 | model4 | 36.47 | 12/12 | 0 | 38 | 14

#### Paired

design | pair | n | geomean | wins-losses(-ties) | sign p
---|---|---|---|---|---
multi1 | hyb2/table3 | 12 | 0.957 | 3-9 | 0.146
multi1 | hyb2/model4 | 12 | 1.070 | 10-2 | 0.039
multi1 | hyb3/table3 | 12 | 0.925 | 1-11 | 0.006
multi1 | hyb3/model4 | 12 | 1.034 | 8-4 | 0.388
multi1 | hyb4/table3 | 12 | 0.956 | 3-9 | 0.146
multi1 | hyb4/model4 | 12 | 1.069 | 10-2 | 0.039
multi1 | hyb6/table3 | 12 | 0.959 | 5-7 | 0.774
multi1 | hyb6/model4 | 12 | 1.072 | 10-2 | 0.039

### multi1 per seed (MHz)

seed | hyb2 | hyb3 | hyb4 | hyb6 | table3 | linear3 | model4
---|---|---|---|---|---|---|---|
1 | 40.37 | 41.41 | 38.29 | 37.81 | 37.34 | 33.02 | 34.08
2 | 39.07 | 37.38 | 36.84 | 39.11 | 37.68 | 28.35 | 34.62
3 | 39.49 | 30.85 | 35.66 | 41.93 | 40.22 | 31.97 | 36.82
4 | 37.58 | 40.07 | 40.30 | 36.11 | 40.40 | 32.68 | 37.60
5 | 37.31 | 38.14 | 40.14 | 37.04 | 42.68 | 34.02 | 36.11
6 | 37.29 | 36.59 | 40.76 | 39.20 | 41.70 | 34.97 | 36.04
7 | 39.98 | 37.02 | 40.36 | 40.63 | 40.21 | 35.10 | 37.75
8 | 38.91 | 36.80 | 38.05 | 38.07 | 43.69 | 32.68 | 32.85
9 | 37.23 | 38.39 | 40.72 | 35.83 | 42.15 | 32.82 | 39.75
10 | 40.12 | 39.97 | 38.39 | 40.61 | 40.78 | 35.69 | 36.85
11 | 38.10 | 36.11 | 40.89 | 39.67 | 40.56 | 35.40 | 34.65
12 | 39.99 | 38.06 | 35.08 | 40.91 | 39.18 | 32.94 | 38.43


No radius is within 2% of table3 (all 0.956-0.959, 3-9 to 5-7); radius makes no measurable difference on
multi1 (hyb3's 0.925 is driven by seed 3, 30.85 MHz). All beat model4 (1.03-1.07). Picked the best by
geomean and wins, hyb6, for Stage B.

## Stage B: hyb6 on dmg_soc (seeds 1-8), corescore25 and vexii (seeds 1-5)

#### Per variant

design | variant | Fmax median MHz | finished/runs | timeouts | median wall s | median router2 s
---|---|---|---|---|---|---
bench_dmg_soc | hyb6 | 29.26 | 8/8 | 0 | 472 | 40
bench_dmg_soc | table3 | 28.77 | 8/8 | 0 | 702 | 78
bench_dmg_soc | linear3 | 26.34 | 8/8 | 0 | 1181 | 121
bench_dmg_soc | model4 | 27.22 | 8/8 | 0 | 599 | 60
bench_corescore25 | hyb6 | 25.94 | 5/5 | 0 | 161 | 47
bench_corescore25 | table3 | 25.92 | 5/5 | 0 | 340 | 83
bench_corescore25 | linear3 | 21.34 | 5/5 | 0 | 343 | 132
bench_corescore25 | model4 | 23.52 | 5/5 | 0 | 280 | 86
bench_vexii | hyb6 | 26.00 | 5/5 | 0 | 116 | 27
bench_vexii | table3 | 26.63 | 5/5 | 0 | 211 | 50
bench_vexii | linear3 | 24.04 | 5/5 | 0 | 377 | 118
bench_vexii | model4 | 24.14 | 5/5 | 0 | 205 | 54

#### Paired

design | pair | n | geomean | wins-losses(-ties) | sign p
---|---|---|---|---|---
bench_dmg_soc | hyb6/table3 | 8 | 1.022 | 5-3 | 0.727
bench_dmg_soc | hyb6/model4 | 8 | 1.048 | 5-2-1 | 0.453
bench_dmg_soc | hyb6/linear3 | 8 | 1.130 | 8-0 | 0.008
bench_corescore25 | hyb6/table3 | 5 | 0.975 | 2-3 | 1.000
bench_corescore25 | hyb6/model4 | 5 | 1.091 | 4-1 | 0.375
bench_corescore25 | hyb6/linear3 | 5 | 1.191 | 5-0 | 0.062
bench_vexii | hyb6/table3 | 5 | 0.986 | 2-3 | 1.000
bench_vexii | hyb6/model4 | 5 | 1.051 | 4-1 | 0.375
bench_vexii | hyb6/linear3 | 5 | 1.069 | 4-1 | 0.375

### bench_dmg_soc per seed (MHz, cc_bufg_inst_O)

seed | hyb6 | table3 | linear3 | model4
---|---|---|---|---|
1 | 28.48 | 29.67 | 26.84 | 26.93
2 | 29.20 | 28.41 | 28.27 | 26.48
3 | 29.32 | 27.47 | 26.22 | 29.32
4 | 30.79 | 26.33 | 23.30 | 26.53
5 | 28.15 | 28.19 | 24.33 | 26.61
6 | 29.50 | 29.14 | 26.46 | 27.51
7 | 29.56 | 29.47 | 24.00 | 30.32
8 | 28.47 | 29.87 | 27.57 | 29.21

### bench_corescore25 per seed (MHz)

seed | hyb6 | table3 | linear3 | model4
---|---|---|---|---|
1 | 26.27 | 25.92 | 21.87 | 23.52
2 | 24.93 | 29.43 | 21.34 | 24.04
3 | 25.94 | 26.81 | 20.48 | 22.80
4 | 25.46 | 25.52 | 21.08 | 25.78
5 | 26.02 | 24.51 | 23.34 | 21.92

### bench_vexii per seed (MHz)

seed | hyb6 | table3 | linear3 | model4
---|---|---|---|---|
1 | 24.20 | 24.12 | 24.04 | 24.02
2 | 27.62 | 24.95 | 24.01 | 25.88
3 | 26.00 | 27.39 | 24.06 | 24.06
4 | 24.00 | 26.63 | 24.02 | 24.26
5 | 26.92 | 27.45 | 24.06 | 24.14


### Pooled (per-seed geomean over designs)

pair | seeds | geomean | wins-losses | sign p
---|---|---|---|---
hyb6/table3, Stage B (dmg_soc, corescore25, vexii) | 18 | 0.999 | 9-9 | 1.000
hyb6/table3, all four designs incl. multi1 | 30 | 0.983 | 14-16 | 0.856
hyb6/model4, Stage B | 18 | 1.061 | 13-4 | 0.049
hyb6/model4, all four designs | 30 | 1.065 | 23-6 | 0.002
hyb6/linear3, Stage B | 18 | 1.129 | 17-1 | <0.001

## Verdict

hyb6 matches the radius-12 tables on the Stage B designs (0.999, 9-9; dmg_soc 1.022, 5-3) with 24 MB of
tables instead of 89 MB, and is clearly better than the model alone (1.065 over 30 seeds, p = 0.002). On
multi1 it stays 4% below table3 at every radius 2..6, so the remaining gap there is not about the near-field
radius; since radius 2 is as good as 6 on multi1, a smaller radius (hyb2/hyb4, 3.6/11.6 MB) is a reasonable
candidate but was not run on Stage B. The build time barely shrinks (3.5 vs 3.9 s idle) unless `build()`
skips the whole-chip slope search in hybrid mode.
