# Hybrid delay estimate, second round (`nextpnr-himbaechel.distance_hybrid2.exe`)

hybN = `--vopt lookahead=hybrid --vopt place-delay=hybrid --vopt hybrid-radius=N`; hyb4m adds
`--vopt hybrid-missing=model`. Same binary for all new runs, `--chipdb` a copy of chipdb-CCGM1A1.bin, `-j 8` on a
32-thread machine. References: table3/linear3 `runs_base3`, model4 `runs_model4`, hyb6 (and hyb4old) `runs_hybrid`
(other binaries, dates, load). Fmax: final report; dmg_soc primary clock `cc_bufg_inst_O`, others the
highest-constraint clock. Ratio = per-seed Fmax a/b, geometric mean; ties apart, two-sided sign test.
hyb4 reproduces runs_hybrid's hyb4 exactly on all 12 multi1 seeds (the build change leaves the tables unchanged).

## 1. Table build time

Before, every build ran a whole-chip Dijkstra for the extrapolation slopes as one more job next to the
8 phases x 13 anchors radius-limited searches; it alone took 3.1 s and set the wall time. Breakdown at radius 4
(multi1 `--pack-only`, idle, 32 threads; seconds):

phase | before | after (hybrid)
---|---|---
anchor wire index per tile type | 0.000 | 0.000
sample tiles per phase | 0.000 | 0.000
"inside" tables and hops per tile type | 0.001 | 0.001
slots and value array (11.6 MB) | 0.002 | 0.002
Dijkstra jobs (wall) | 3.311 | 0.29
  of which whole-chip slope job | 3.08 | skipped
  sum of the 104 radius jobs (CPU) | 8.0 | 8.0
slopes, remap | 0.000 | 0.000
**total (log line)** | **3.3-3.4** | **0.3**

Hybrid builds after the change (idle): radius 2 0.2 s (5.5 CPU-s), 4 0.3 s (8.0), 6 0.4 s (11.7), 12 1.0-1.8 s
(28-38). Under the `-j 8` benchmark load: radius 4 1.1-1.3 s, radius 12 1.1-8.6 s. `table` mode is unchanged
(still runs the slope job: radius 12 3.9-4.0 s, slopes 31.0/30.6 ps per tile on multi1). The whole cost left is the
radius-limited searches, which cover radius + 8 tiles (MARGIN), so radius 2 still searches 21x21 tiles.

Value check: `lookup()` on 10,000 deterministic random in-radius (CPE anchor tile, anchor, normalised wire)
pairs, old build vs new, hybrid and table mode, radius 2, 4 and 12: all six dumps byte-identical (and table r4 ==
hybrid r4). 17.8% of the r4 samples are UNREACHABLE (20000), 0.5% -1 (no remap).

## 2. Edge effect (radius 4 vs radius 12, all in-table pairs of the 8 sample tiles)

ring = max(|dx|, |dy|). over = r4 > r12; mean excess over all pairs / over the overestimated ones.
missing = no value in r4 (UNREACHABLE, 20 ns) but a value in r12. r4 < r12 never occurs.

ring | pairs | over | over frac | mean excess ps | excess of over ps | missing (frac)
---|---|---|---|---|---|---
0 | 41,088 | 91 | 0.22% | 0.9 | 412 | 1,344 (3.2%)
1 | 326,568 | 794 | 0.24% | 0.8 | 342 | 10,968 (3.2%)
2 | 651,776 | 1,545 | 0.24% | 0.4 | 159 | 20,016 (3.0%)
3 | 977,176 | 2,476 | 0.25% | 0.5 | 188 | 31,368 (3.1%)
4 | 1,306,008 | 8,276 | 0.63% | 1.2 | 190 | 35,592 (2.7%)

The outer ring is overestimated a bit more often (0.63% vs 0.24% of pairs) but by 1.2 ps on average: no
edge effect worth a `hybrid-trust` option (not added). radius 2 and 6 against 12 look the same (outer ring
0.47% / 0.39%, 1.2 / 1.0 ps). The larger finding is the "missing" column: about 3% of the wires at every
ring, including ring 0, reach the pin only through a detour beyond radius + 8 (examples: `SB_DRIVE.Pxx.D3.IN`,
`D4.IN` next to the CPE, 1.0-3.5 ns in the r12 table, mean 2.7 ns); the r4 table calls them unreachable
(20 ns). It is the same at every radius 2-6 since the margin is fixed. `--vopt hybrid-missing=model` sends
these (and all other MISSING wires) to the model instead; tested as hyb4m below.

## 3. Benchmarks

### Stage B (dmg_soc 1-8, corescore25 and vexii 1-5): Per variant

design | variant | Fmax median MHz | finished/runs | timeouts | median wall s | median router2 s
---|---|---|---|---|---|---
bench_dmg_soc | hyb2 | 28.44 | 8/8 | 0 | 597 | 38
bench_dmg_soc | hyb4 | 29.38 | 8/8 | 0 | 420 | 33
bench_dmg_soc | hyb4m | 28.79 | 8/8 | 0 | 293 | 34
bench_dmg_soc | hyb12 | 29.50 | 8/8 | 0 | 366 | 29
bench_dmg_soc | hyb6 | 29.26 | 8/8 | 0 | 472 | 40
bench_dmg_soc | table3 | 28.77 | 8/8 | 0 | 702 | 78
bench_dmg_soc | linear3 | 26.34 | 8/8 | 0 | 1181 | 121
bench_dmg_soc | model4 | 27.22 | 8/8 | 0 | 599 | 60
bench_corescore25 | hyb2 | 25.44 | 5/5 | 0 | 158 | 49
bench_corescore25 | hyb4 | 26.02 | 5/5 | 0 | 173 | 56
bench_corescore25 | hyb4m | 25.49 | 5/5 | 0 | 164 | 49
bench_corescore25 | hyb12 | 25.88 | 5/5 | 0 | 181 | 52
bench_corescore25 | hyb6 | 25.94 | 5/5 | 0 | 161 | 47
bench_corescore25 | table3 | 25.92 | 5/5 | 0 | 340 | 83
bench_corescore25 | linear3 | 21.34 | 5/5 | 0 | 343 | 132
bench_corescore25 | model4 | 23.52 | 5/5 | 0 | 280 | 86
bench_vexii | hyb2 | 25.00 | 5/5 | 0 | 157 | 34
bench_vexii | hyb4 | 24.50 | 4/5 | 1 | 153 | 40
bench_vexii | hyb4m | 24.39 | 5/5 | 0 | 155 | 43
bench_vexii | hyb12 | 25.45 | 4/5 | 1 | 135 | 36
bench_vexii | hyb6 | 26.00 | 5/5 | 0 | 116 | 27
bench_vexii | table3 | 26.63 | 5/5 | 0 | 211 | 50
bench_vexii | linear3 | 24.04 | 5/5 | 0 | 377 | 118
bench_vexii | model4 | 24.14 | 5/5 | 0 | 205 | 54

### Stage B (dmg_soc 1-8, corescore25 and vexii 1-5): Paired

design | pair | n | geomean | wins-losses(-ties) | sign p
---|---|---|---|---|---
bench_dmg_soc | hyb2/table3 | 8 | 1.001 | 2-6 | 0.289
bench_dmg_soc | hyb2/model4 | 8 | 1.026 | 6-2 | 0.289
bench_dmg_soc | hyb2/hyb6 | 8 | 0.979 | 2-6 | 0.289
bench_dmg_soc | hyb2/linear3 | 8 | 1.106 | 8-0 | 0.008
bench_dmg_soc | hyb4/table3 | 8 | 1.014 | 5-3 | 0.727
bench_dmg_soc | hyb4/model4 | 8 | 1.040 | 6-2 | 0.289
bench_dmg_soc | hyb4/hyb6 | 8 | 0.992 | 4-4 | 1.000
bench_dmg_soc | hyb4/linear3 | 8 | 1.121 | 8-0 | 0.008
bench_dmg_soc | hyb4m/table3 | 8 | 1.000 | 3-5 | 0.727
bench_dmg_soc | hyb4m/model4 | 8 | 1.025 | 6-2 | 0.289
bench_dmg_soc | hyb4m/hyb6 | 8 | 0.978 | 2-6 | 0.289
bench_dmg_soc | hyb4m/linear3 | 8 | 1.105 | 8-0 | 0.008
bench_dmg_soc | hyb12/table3 | 8 | 1.031 | 5-3 | 0.727
bench_dmg_soc | hyb12/model4 | 8 | 1.057 | 7-1 | 0.070
bench_dmg_soc | hyb12/hyb6 | 8 | 1.009 | 4-3-1 | 1.000
bench_dmg_soc | hyb12/linear3 | 8 | 1.140 | 8-0 | 0.008
bench_dmg_soc | hyb4m/hyb4 | 8 | 0.986 | 1-7 | 0.070
bench_dmg_soc | hyb2/hyb4 | 8 | 0.987 | 3-5 | 0.727
bench_dmg_soc | hyb12/hyb4 | 8 | 1.017 | 6-2 | 0.289
bench_corescore25 | hyb2/table3 | 5 | 0.958 | 2-3 | 1.000
bench_corescore25 | hyb2/model4 | 5 | 1.072 | 3-2 | 1.000
bench_corescore25 | hyb2/hyb6 | 5 | 0.983 | 1-4 | 0.375
bench_corescore25 | hyb2/linear3 | 5 | 1.170 | 5-0 | 0.062
bench_corescore25 | hyb4/table3 | 5 | 0.994 | 2-3 | 1.000
bench_corescore25 | hyb4/model4 | 5 | 1.113 | 5-0 | 0.062
bench_corescore25 | hyb4/hyb6 | 5 | 1.020 | 3-2 | 1.000
bench_corescore25 | hyb4/linear3 | 5 | 1.214 | 5-0 | 0.062
bench_corescore25 | hyb4m/table3 | 5 | 0.992 | 2-3 | 1.000
bench_corescore25 | hyb4m/model4 | 5 | 1.110 | 4-1 | 0.375
bench_corescore25 | hyb4m/hyb6 | 5 | 1.018 | 2-3 | 1.000
bench_corescore25 | hyb4m/linear3 | 5 | 1.212 | 5-0 | 0.062
bench_corescore25 | hyb12/table3 | 5 | 0.992 | 3-2 | 1.000
bench_corescore25 | hyb12/model4 | 5 | 1.110 | 4-1 | 0.375
bench_corescore25 | hyb12/hyb6 | 5 | 1.018 | 3-2 | 1.000
bench_corescore25 | hyb12/linear3 | 5 | 1.212 | 5-0 | 0.062
bench_corescore25 | hyb4m/hyb4 | 5 | 0.998 | 1-4 | 0.375
bench_corescore25 | hyb2/hyb4 | 5 | 0.963 | 1-4 | 0.375
bench_corescore25 | hyb12/hyb4 | 5 | 0.998 | 2-3 | 1.000
bench_vexii | hyb2/table3 | 5 | 0.955 | 2-3 | 1.000
bench_vexii | hyb2/model4 | 5 | 1.017 | 4-1 | 0.375
bench_vexii | hyb2/hyb6 | 5 | 0.968 | 2-3 | 1.000
bench_vexii | hyb2/linear3 | 5 | 1.035 | 5-0 | 0.062
bench_vexii | hyb4/table3 | 4 | 0.952 | 1-3 | 0.625
bench_vexii | hyb4/model4 | 4 | 0.999 | 3-1 | 0.625
bench_vexii | hyb4/hyb6 | 4 | 0.965 | 2-2 | 1.000
bench_vexii | hyb4/linear3 | 4 | 1.020 | 4-0 | 0.125
bench_vexii | hyb4m/table3 | 5 | 0.946 | 1-4 | 0.375
bench_vexii | hyb4m/model4 | 5 | 1.009 | 4-1 | 0.375
bench_vexii | hyb4m/hyb6 | 5 | 0.960 | 2-3 | 1.000
bench_vexii | hyb4m/linear3 | 5 | 1.027 | 5-0 | 0.062
bench_vexii | hyb12/table3 | 4 | 0.973 | 2-2 | 1.000
bench_vexii | hyb12/model4 | 4 | 1.063 | 3-1 | 0.625
bench_vexii | hyb12/hyb6 | 4 | 1.015 | 3-1 | 0.625
bench_vexii | hyb12/linear3 | 4 | 1.066 | 4-0 | 0.125
bench_vexii | hyb4m/hyb4 | 4 | 1.010 | 3-1 | 0.625
bench_vexii | hyb2/hyb4 | 4 | 1.011 | 3-1 | 0.625
bench_vexii | hyb12/hyb4 | 3 | 1.032 | 2-1 | 1.000

### bench_dmg_soc per seed (MHz, cc_bufg_inst_O)

seed | hyb2 | hyb4 | hyb4m | hyb12 | hyb6 | table3 | linear3 | model4
---|---|---|---|---|---|---|---|---|
1 | 30.09 | 29.75 | 29.59 | 29.01 | 28.48 | 29.67 | 26.84 | 26.93
2 | 28.39 | 29.78 | 28.77 | 30.32 | 29.20 | 28.41 | 28.27 | 26.48
3 | 27.04 | 27.38 | 27.59 | 29.77 | 29.32 | 27.47 | 26.22 | 29.32
4 | 28.07 | 27.77 | 27.56 | 29.33 | 30.79 | 26.33 | 23.30 | 26.53
5 | 28.02 | 28.04 | 27.32 | 28.15 | 28.15 | 28.19 | 24.33 | 26.61
6 | 28.67 | 29.25 | 28.80 | 29.48 | 29.50 | 29.14 | 26.46 | 27.51
7 | 28.48 | 30.11 | 29.32 | 29.52 | 29.56 | 29.47 | 24.00 | 30.32
8 | 29.85 | 29.52 | 29.43 | 29.85 | 28.47 | 29.87 | 27.57 | 29.21

### bench_corescore25 per seed (MHz)

seed | hyb2 | hyb4 | hyb4m | hyb12 | hyb6 | table3 | linear3 | model4
---|---|---|---|---|---|---|---|---|
1 | 26.59 | 25.60 | 25.49 | 27.46 | 26.27 | 25.92 | 21.87 | 23.52
2 | 23.61 | 27.77 | 26.96 | 25.81 | 24.93 | 29.43 | 21.34 | 24.04
3 | 24.82 | 25.60 | 27.75 | 27.37 | 25.94 | 26.81 | 20.48 | 22.80
4 | 25.44 | 26.02 | 25.39 | 24.50 | 25.46 | 25.52 | 21.08 | 25.78
5 | 26.01 | 26.24 | 25.39 | 25.88 | 26.02 | 24.51 | 23.34 | 21.92

### bench_vexii per seed (MHz)

seed | hyb2 | hyb4 | hyb4m | hyb12 | hyb6 | table3 | linear3 | model4
---|---|---|---|---|---|---|---|---|
1 | 24.66 | 24.31 | 24.39 | 24.67 | 24.20 | 24.12 | 24.04 | 24.02
2 | 25.41 | 24.14 | 24.04 | T/O | 27.62 | 24.95 | 24.01 | 25.88
3 | 24.11 | 24.69 | 25.33 | 27.69 | 26.00 | 27.39 | 24.06 | 24.06
4 | 25.00 | 24.92 | 25.28 | 24.10 | 24.00 | 26.63 | 24.02 | 24.26
5 | 25.28 | T/O | 24.37 | 26.24 | 26.92 | 27.45 | 24.06 | 24.14

### Stage B (dmg_soc 1-8, corescore25 and vexii 1-5): Pooled over the designs

pair | seeds | geomean | wins-losses | sign p
---|---|---|---|---
hyb2/table3 | 18 | 0.976 | 6-12 | 0.238
hyb2/model4 | 18 | 1.036 | 13-5 | 0.096
hyb2/hyb6 | 18 | 0.977 | 5-13 | 0.096
hyb2/linear3 | 18 | 1.103 | 18-0 | 0.000
hyb4/table3 | 17 | 0.993 | 8-9 | 1.000
hyb4/model4 | 17 | 1.051 | 14-3 | 0.013
hyb4/hyb6 | 17 | 0.993 | 9-8 | 1.000
hyb4/linear3 | 17 | 1.122 | 17-0 | 0.000
hyb4m/table3 | 18 | 0.982 | 6-12 | 0.238
hyb4m/model4 | 18 | 1.044 | 14-4 | 0.031
hyb4m/hyb6 | 18 | 0.984 | 6-12 | 0.238
hyb4m/linear3 | 18 | 1.111 | 18-0 | 0.000
hyb12/table3 | 17 | 1.005 | 10-7 | 0.629
hyb12/model4 | 17 | 1.074 | 14-3 | 0.013
hyb12/hyb6 | 17 | 1.013 | 10-6 | 0.454
hyb12/linear3 | 17 | 1.142 | 17-0 | 0.000
hyb4m/hyb4 | 17 | 0.995 | 5-12 | 0.143
hyb2/hyb4 | 17 | 0.986 | 7-10 | 0.629
hyb12/hyb4 | 16 | 1.014 | 10-6 | 0.454


### Stage A (multi1 1-12): Per variant

design | variant | Fmax median MHz | finished/runs | timeouts | median wall s | median router2 s
---|---|---|---|---|---|---
multi1 | hyb12 | 41.13 | 12/12 | 0 | 38 | 13
multi1 | hyb8 | 39.20 | 12/12 | 0 | 31 | 11
multi1 | hyb4 | 39.27 | 12/12 | 0 | 37 | 14
multi1 | hyb4m | 40.29 | 12/12 | 0 | 38 | 14
multi1 | hyb6 | 39.16 | 12/12 | 0 | 41 | 14
multi1 | hyb4old | 39.27 | 12/12 | 0 | 41 | 14
multi1 | table3 | 40.48 | 12/12 | 0 | 37 | 11
multi1 | model4 | 36.47 | 12/12 | 0 | 38 | 14

### Stage A (multi1 1-12): Paired

design | pair | n | geomean | wins-losses(-ties) | sign p
---|---|---|---|---|---
multi1 | hyb12/table3 | 12 | 1.005 | 7-5 | 0.774
multi1 | hyb12/hyb6 | 12 | 1.047 | 10-2 | 0.039
multi1 | hyb12/model4 | 12 | 1.123 | 11-1 | 0.006
multi1 | hyb8/table3 | 12 | 0.977 | 4-8 | 0.388
multi1 | hyb8/hyb6 | 12 | 1.019 | 7-5 | 0.774
multi1 | hyb8/model4 | 12 | 1.092 | 11-1 | 0.006
multi1 | hyb4/table3 | 12 | 0.956 | 3-9 | 0.146
multi1 | hyb4/hyb6 | 12 | 0.997 | 6-6 | 1.000
multi1 | hyb4/model4 | 12 | 1.069 | 10-2 | 0.039
multi1 | hyb4m/table3 | 12 | 0.986 | 6-6 | 1.000
multi1 | hyb4m/hyb6 | 12 | 1.028 | 9-3 | 0.146
multi1 | hyb4m/model4 | 12 | 1.102 | 10-2 | 0.039
multi1 | hyb4m/hyb4 | 12 | 1.031 | 9-3 | 0.146
multi1 | hyb12/hyb4 | 12 | 1.051 | 10-2 | 0.039
multi1 | hyb8/hyb4 | 12 | 1.022 | 7-5 | 0.774
multi1 | hyb4/hyb4old | 12 | 1.000 | 0-0-12 | nan

### multi1 per seed (MHz)

seed | hyb12 | hyb8 | hyb4 | hyb4m | hyb6 | hyb4old | table3 | model4
---|---|---|---|---|---|---|---|---|
1 | 40.06 | 41.25 | 38.29 | 38.36 | 37.81 | 38.29 | 37.34 | 34.08
2 | 34.76 | 38.80 | 36.84 | 38.59 | 39.11 | 36.84 | 37.68 | 34.62
3 | 42.27 | 39.60 | 35.66 | 40.61 | 41.93 | 35.66 | 40.22 | 36.82
4 | 42.48 | 43.60 | 40.30 | 39.41 | 36.11 | 40.30 | 40.40 | 37.60
5 | 41.07 | 37.80 | 40.14 | 41.49 | 37.04 | 40.14 | 42.68 | 36.11
6 | 40.68 | 39.68 | 40.76 | 39.97 | 39.20 | 40.76 | 41.70 | 36.04
7 | 41.50 | 37.51 | 40.36 | 41.49 | 40.63 | 40.36 | 40.21 | 37.75
8 | 41.00 | 38.76 | 38.05 | 40.78 | 38.07 | 38.05 | 43.69 | 32.85
9 | 45.03 | 40.07 | 40.72 | 37.21 | 35.83 | 40.72 | 42.15 | 39.75
10 | 42.87 | 38.69 | 38.39 | 41.18 | 40.61 | 38.39 | 40.78 | 36.85
11 | 41.19 | 38.72 | 40.89 | 43.69 | 39.67 | 40.89 | 40.56 | 34.65
12 | 36.56 | 40.93 | 35.08 | 37.04 | 40.91 | 35.08 | 39.18 | 38.43


### Pooled over all four designs (multi1, dmg_soc, corescore25, vexii; timeouts excluded)

pair | seeds | geomean | wins-losses | sign p
---|---|---|---|---
hyb12/table3 | 29 | 1.005 | 17-12 | 0.458
hyb12/model4 | 29 | 1.094 | 25-4 | <0.001
hyb12/hyb4 | 28 | 1.030 | 20-8 | 0.036
hyb4/table3 | 29 | 0.978 | 11-18 | 0.265
hyb4/model4 | 29 | 1.058 | 24-5 | 0.001
hyb4m/hyb4 | 29 | 1.010 | 14-15 | 1.000
hyb4m/table3 | 30 | 0.984 | 12-18 | 0.362

Timeouts (3600 s, router never converged): vexii hyb4 seed 5, vexii hyb12 seed 2 (none in the earlier
hyb6/table3/model4/linear3 runs of vexii).

## 4. Same-load timing (dmg_soc seed 1, one run at a time, idle machine, same binary)

variant | Fmax MHz | wall s | table build s | HeAP s | SA s | router2 s | clock router s | post-route repair rounds
---|---|---|---|---|---|---|---|---
table (r12, extrapolation) | 29.67 | 947 | 3.9 | 25.1 | 55.1 | 21.9 | 16.8 | 3
hyb4 | 29.75 | 253 | 0.3 | 23.3 | 51.0 | 22.9 | 18.4 | 2
model | 26.93 | 221 | - | 23.1 | 39.4 | 29.2 | 16.9 | 2

Fmax equal to the Stage B / runs_base3 / runs_model4 values of seed 1 (deterministic). The logged phases differ
little; the wall-time gap (947 vs 253 s) is in the post-route passes (place-repair / LUT fusing / routing repair;
not timed in the log): table did 3 rounds of them, hyb4 and model 2. This is one seed and does not show that
hybrid is generally faster.

## Verdict

* The multi1 gap was the radius, not the far-field model: hyb12 (radius 12, model beyond) is level with table3
  on multi1 (1.005, 7-5) and 1.051 over hyb4 (10-2, p 0.039); hyb8 lies between (0.977 of table3). The
  near field the model gets wrong extends past 4-6 tiles on multi1.
* Over all four designs hyb12 is level with table3 (1.005, 17-12) and 1.094 over model4; hyb4 is 0.978 of table3
  and 1.058 over model4; hyb12/hyb4 = 1.030 (20-8, p 0.036). On Stage B alone radius barely matters (hyb12/hyb4
  1.014, hyb2/hyb4 0.986).
* hybrid-missing=model helps multi1 (1.031 over hyb4, 9-3) but not Stage B (dmg_soc 0.986, 1-7): kept as an
  option, default off.
* Recommended default: `hybrid-radius=12` (89 MB, 1.0 s idle / 28 CPU-s, 1-9 s under load) if that memory is
  acceptable; otherwise 4 (11.6 MB, 0.3 s), at about 2% Fmax (4% on multi1).
