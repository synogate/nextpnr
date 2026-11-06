# Distance model step 4 (`nextpnr-himbaechel.distance_model4.exe`): model4 vs table3, linear3, model3

model4 = closed-form model, lookahead=model place-delay=model, no tables. table3/linear3 from runs_base3 (main-based baseline, lookahead/place-delay table resp. linear); model3 from runs_model3. Fmax: final report; dmg_soc primary clock cc_bufg_inst_O (33 MHz PLL), others the highest-constraint clock. Ratio = per-seed Fmax a/b over seeds finished on both sides, geometric mean; ties apart, two-sided sign test on wins/losses. Seeds: dmg_soc 1-8, bench designs 1-5, multi1 1-12.

## Paired ratios

design | ratio | n paired | geomean | win:loss(:tie) | sign p
---|---|---|---|---|---
bench_dmg_soc | model4/table3 | 8 | 0.975 | 3:5 | 0.727
bench_dmg_soc | model4/linear3 | 8 | 1.078 | 7:1 | 0.070
bench_dmg_soc | model4/model3 | 8 | 1.017 | 4:4 | 1.000
bench_corescore25 | model4/table3 | 5 | 0.894 | 1:4 | 0.375
bench_corescore25 | model4/linear3 | 5 | 1.091 | 4:1 | 0.375
bench_corescore25 | model4/model3 | 3 | 1.033 | 1:2 | 1.000
bench_mandelbrot | model4/table3 | 5 | 0.953 | 1:4 | 0.375
bench_mandelbrot | model4/linear3 | 5 | 1.082 | 4:1 | 0.375
bench_mandelbrot | model4/model3 | 5 | 0.992 | 2:3 | 1.000
bench_minerva | model4/table3 | 3 | 1.012 | 2:1 | 1.000
bench_minerva | model4/linear3 | 0 | - | - | -
bench_minerva | model4/model3 | 3 | 1.004 | 2:1 | 1.000
bench_neorv32 | model4/table3 | 5 | 1.005 | 4:0:1 | 0.125
bench_neorv32 | model4/linear3 | 5 | 1.010 | 3:2 | 1.000
bench_neorv32 | model4/model3 | 5 | 1.000 | 1:1:3 | 1.000
bench_picorv32 | model4/table3 | 4 | 0.976 | 1:3 | 0.625
bench_picorv32 | model4/linear3 | 4 | 1.082 | 4:0 | 0.125
bench_picorv32 | model4/model3 | 4 | 1.001 | 1:3 | 0.625
bench_vexii | model4/table3 | 5 | 0.938 | 1:4 | 0.375
bench_vexii | model4/linear3 | 5 | 1.018 | 3:1:1 | 0.625
bench_vexii | model4/model3 | 5 | 0.978 | 1:4 | 0.375
bench_vexriscv | model4/table3 | 5 | 1.001 | 2:3 | 1.000
bench_vexriscv | model4/linear3 | 4 | 1.023 | 3:1 | 0.625
bench_vexriscv | model4/model3 | 5 | 1.004 | 2:3 | 1.000
bench_vexsmp2 | model4/table3 | 3 | 0.865 | 0:3 | 0.250
bench_vexsmp2 | model4/linear3 | 0 | - | - | -
bench_vexsmp2 | model4/model3 | 4 | 0.996 | 2:2 | 1.000
multi1 | model4/table3 | 12 | 0.895 | 0:12 | 0.000
multi1 | model4/linear3 | 12 | 1.090 | 11:1 | 0.006
multi1 | model4/model3 | 12 | 0.982 | 4:8 | 0.388

## Per variant (median Fmax MHz, finished/existing logs; median wall s)

design | model4 Fmax (ok/runs) | model3 Fmax (ok/runs) | table3 Fmax (ok/runs) | linear3 Fmax (ok/runs) | model4 wall s | model3 wall s | table3 wall s | linear3 wall s
---|---|---|---|---|---|---|---|---|
bench_dmg_soc | 27.22 (8/8) | 27.60 (8/8) | 28.77 (8/8) | 26.34 (8/8) | 599 | 750 | 702 | 1181
bench_corescore25 | 23.52 (5/5) | 22.99 (3/5) | 25.92 (5/5) | 21.34 (5/5) | 280 | 355 | 340 | 343
bench_mandelbrot | 61.99 (5/5) | 63.04 (5/5) | 65.04 (5/5) | 53.61 (5/5) | 109 | 128 | 92 | 76
bench_minerva | 16.89 (3/5) | 16.78 (5/5) | 16.68 (5/5) | 14.55 (1/5) | 1292 | 1700 | 517 | 3420
bench_neorv32 | 19.54 (5/5) | 19.54 (5/5) | 19.51 (5/5) | 19.47 (5/5) | 159 | 188 | 152 | 187
bench_picorv32 | 27.25 (4/5) | 26.80 (5/5) | 27.37 (5/5) | 24.68 (5/5) | 145 | 175 | 108 | 163
bench_vexii | 24.14 (5/5) | 24.56 (5/5) | 26.63 (5/5) | 24.04 (5/5) | 205 | 267 | 211 | 377
bench_vexriscv | 24.08 (5/5) | 24.12 (5/5) | 24.08 (5/5) | 23.76 (4/5) | 246 | 562 | 184 | 2428
bench_vexsmp2 | 12.73 (4/5) | 12.81 (4/5) | 14.56 (4/5) | - (0/5) | 1743 | 1115 | 1846 | -
multi1 | 36.47 (12/12) | 37.42 (12/12) | 40.48 (12/12) | 32.98 (12/12) | 38 | 50 | 37 | 44

## Missing / unfinished / anomalies

- bench_corescore25 model3 s2 timeout
- bench_corescore25 model3 s5 timeout
- bench_minerva model4 s1 timeout
- bench_minerva model4 s4 timeout
- bench_minerva linear3 s2 timeout
- bench_minerva linear3 s3 timeout
- bench_minerva linear3 s4 timeout
- bench_minerva linear3 s5 timeout
- bench_picorv32 model4 s2 timeout
- bench_vexriscv linear3 s3 timeout
- bench_vexsmp2 model4 s5 timeout
- bench_vexsmp2 model3 s5 timeout
- bench_vexsmp2 table3 s3 timeout
- bench_vexsmp2 linear3 s1 timeout
- bench_vexsmp2 linear3 s2 timeout
- bench_vexsmp2 linear3 s3 timeout
- bench_vexsmp2 linear3 s4 timeout
- bench_vexsmp2 linear3 s5 timeout

## Verdict

1. model4 vs model3: a wash. Geomeans 0.978-1.033, no design significant (dmg_soc 1.017 at 4:4, multi1 0.982 at 4:8 with p 0.39); not demonstrably better.
2. model4 is faster than model3 in wall time on 9 of 10 designs (dmg_soc 599 s vs 750 s); vexsmp2 is the exception (1743 s vs 1115 s).
3. Gap to table3: dmg_soc -2.5% (3:5, n.s.); clearly worse on multi1 (-10.5%, 0:12, p<0.001), corescore25 (-11%), vexsmp2 (-13.5%) and vexii (-6%); about even on minerva, neorv32 and vexriscv.
4. Better than linear3 everywhere it can be paired: dmg_soc +7.8% (7:1, p 0.07), multi1 +9.0% (11:1, p 0.006), +8-9% on corescore25, mandelbrot and picorv32.
5. Timeouts: model4 4 of 60 runs (minerva s1/s4, picorv32 s2, vexsmp2 s5). The picorv32 timeout is new (model3 and table3 finished all 5). No model4 run built delay tables.
