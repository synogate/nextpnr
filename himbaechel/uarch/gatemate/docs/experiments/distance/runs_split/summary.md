# Placer vs router split of the closed-form model loss (`nextpnr-himbaechel.distance_model4.exe`)

Same binary, same chipdb. table4 = `place-delay=table lookahead=table`; pm_rt = `place-delay=model lookahead=table`; pt_rm = `place-delay=table lookahead=model`. All three build the delay tables, so the post-route repairs use the tables in all of them. model4 (runs_model4/) = `place-delay=model lookahead=model` (no tables, repairs use the model). table3 (runs_base3/, distance_model3 binary) = same options as table4. Fmax: last report; dmg_soc clock cc_bufg_inst_O (33 MHz PLL), others the highest-constraint clock. Ratio = per-seed Fmax a/b, geometric mean; ties apart, two-sided sign test on wins/losses.

## Verdict

1. Both sides lose, and the router side (pt_rm, model lookahead) loses slightly more than the placer side (pm_rt, model predictDelay): multi1 0.937 vs 0.960 (both 1-11, p=0.006), corescore25 0.940 (0-5) vs 0.955 (2-3).
2. The two losses add up almost exactly in log space: multi1 0.960*0.937=0.900 vs model4 0.895; corescore25 0.955*0.940=0.898 vs model4 0.894.
3. dmg_soc (cc_bufg_inst_O) shows no significant effect on either side (pm_rt 1.009 4-4, pt_rm 0.988 2-5, model4 0.975 3-5); the per-seed noise is larger than the model effect.
4. vexsmp2 does not fit the additive picture (pm_rt 0.980 n=2, pt_rm 1.008 n=4, model4 0.865 n=3). Too many timeouts to tell; the gap may be in the repairs, which use the model only in model4.
5. Caveat: model4 also runs the post-route repairs on the model, while pm_rt and pt_rm use the tables. So 'additive' covers placer + router + repairs together. table3 == table4 on every finished seed and clock, so the model4 binary reproduces the old table runs bit-exactly.

## Paired ratios

design | ratio | n | geomean | wins-losses-ties | sign p
---|---|---|---|---|---
bench_dmg_soc | pm_rt/table4 | 8 | 1.009 | 4-4-0 | 1.000
bench_dmg_soc | pt_rm/table4 | 8 | 0.988 | 2-5-1 | 0.453
bench_dmg_soc | model4/table4 | 8 | 0.975 | 3-5-0 | 0.727
bench_dmg_soc | table3/table4 | 8 | 1.000 | 0-0-8 | nan
multi1 | pm_rt/table4 | 12 | 0.960 | 1-11-0 | 0.006
multi1 | pt_rm/table4 | 12 | 0.937 | 1-11-0 | 0.006
multi1 | model4/table4 | 12 | 0.895 | 0-12-0 | 0.000
multi1 | table3/table4 | 12 | 1.000 | 0-0-12 | nan
bench_vexsmp2 | pm_rt/table4 | 2 | 0.980 | 1-1-0 | 1.000
bench_vexsmp2 | pt_rm/table4 | 4 | 1.008 | 3-1-0 | 0.625
bench_vexsmp2 | model4/table4 | 3 | 0.865 | 0-3-0 | 0.250
bench_vexsmp2 | table3/table4 | 4 | 1.000 | 0-0-4 | nan
bench_corescore25 | pm_rt/table4 | 5 | 0.955 | 2-3-0 | 1.000
bench_corescore25 | pt_rm/table4 | 5 | 0.940 | 0-5-0 | 0.062
bench_corescore25 | model4/table4 | 5 | 0.894 | 1-4-0 | 0.375
bench_corescore25 | table3/table4 | 5 | 1.000 | 0-0-5 | nan

## Per variant (medians over finished runs)

design | variant | ok | Fmax median | min-max | route s | wall s | timeouts | tables built
---|---|---|---|---|---|---|---|---
bench_dmg_soc | table4 | 8/8 | 28.77 | 26.33-29.87 | 40.3 | 423.0 | 0 | 8
bench_dmg_soc | pm_rt | 8/8 | 28.62 | 26.96-30.64 | 32.5 | 543.4 | 0 | 8
bench_dmg_soc | pt_rm | 8/8 | 28.41 | 25.92-30.15 | 36.7 | 425.1 | 0 | 8
bench_dmg_soc | model4 | 8/8 | 27.22 | 26.48-30.32 | 59.8 | 599.1 | 0 | 0
bench_dmg_soc | table3 | 8/8 | 28.77 | 26.33-29.87 | 77.7 | 702.4 | 0 | 8
multi1 | table4 | 12/12 | 40.48 | 37.34-43.69 | 11.1 | 36.8 | 0 | 12
multi1 | pm_rt | 12/12 | 38.87 | 36.18-41.54 | 13.2 | 40.3 | 0 | 12
multi1 | pt_rm | 12/12 | 38.13 | 35.12-41.47 | 13.5 | 39.5 | 0 | 12
multi1 | model4 | 12/12 | 36.47 | 32.85-39.75 | 14.2 | 38.1 | 0 | 0
multi1 | table3 | 12/12 | 40.48 | 37.34-43.69 | 10.9 | 37.1 | 0 | 12
bench_vexsmp2 | table4 | 4/5 | 14.56 | 13.58-15.28 | 275.6 | 1113.8 | 1 | 4
bench_vexsmp2 | pm_rt | 3/5 | 13.34 | 12.51-14.49 | 174.3 | 1300.3 | 2 | 3
bench_vexsmp2 | pt_rm | 4/5 | 14.62 | 14.16-14.99 | 199.3 | 656.2 | 1 | 4
bench_vexsmp2 | model4 | 4/5 | 12.73 | 12.50-12.92 | 365.9 | 1743.0 | 1 | 0
bench_vexsmp2 | table3 | 4/5 | 14.56 | 13.58-15.28 | 431.5 | 1846.0 | 1 | 4
bench_corescore25 | table4 | 5/5 | 25.92 | 24.51-29.43 | 63.1 | 199.8 | 0 | 5
bench_corescore25 | pm_rt | 5/5 | 25.41 | 24.56-25.61 | 81.7 | 222.0 | 0 | 5
bench_corescore25 | pt_rm | 5/5 | 25.39 | 22.70-25.68 | 47.4 | 186.8 | 0 | 5
bench_corescore25 | model4 | 5/5 | 23.52 | 21.92-25.78 | 86.1 | 280.2 | 0 | 0
bench_corescore25 | table3 | 5/5 | 25.92 | 24.51-29.43 | 82.7 | 340.1 | 0 | 5

## Per seed Fmax (MHz)

design | seed | table4 | pm_rt | pt_rm | model4 | table3
---|---|---|---|---|---|---|
bench_dmg_soc | 1 | 29.67 | 28.46 | 30.15 | 26.93 | 29.67
bench_dmg_soc | 2 | 28.41 | 27.60 | 27.88 | 26.48 | 28.41
bench_dmg_soc | 3 | 27.47 | 30.64 | 26.02 | 29.32 | 27.47
bench_dmg_soc | 4 | 26.33 | 26.96 | 25.92 | 26.53 | 26.33
bench_dmg_soc | 5 | 28.19 | 29.69 | 28.19 | 26.61 | 28.19
bench_dmg_soc | 6 | 29.14 | 28.03 | 29.11 | 27.51 | 29.14
bench_dmg_soc | 7 | 29.47 | 28.79 | 28.64 | 30.32 | 29.47
bench_dmg_soc | 8 | 29.87 | 30.37 | 29.99 | 29.21 | 29.87
multi1 | 1 | 37.34 | 36.18 | 38.91 | 34.08 | 37.34
multi1 | 2 | 37.68 | 37.48 | 35.12 | 34.62 | 37.68
multi1 | 3 | 40.22 | 39.95 | 38.81 | 36.82 | 40.22
multi1 | 4 | 40.40 | 37.77 | 36.47 | 37.60 | 40.40
multi1 | 5 | 42.68 | 41.43 | 39.88 | 36.11 | 42.68
multi1 | 6 | 41.70 | 38.35 | 37.82 | 36.04 | 41.70
multi1 | 7 | 40.21 | 41.54 | 37.88 | 37.75 | 40.21
multi1 | 8 | 43.69 | 39.24 | 37.10 | 32.85 | 43.69
multi1 | 9 | 42.15 | 38.49 | 41.47 | 39.75 | 42.15
multi1 | 10 | 40.78 | 39.24 | 38.73 | 36.85 | 40.78
multi1 | 11 | 40.56 | 40.04 | 38.38 | 34.65 | 40.56
multi1 | 12 | 39.18 | 37.32 | 35.32 | 38.43 | 39.18
bench_vexsmp2 | 1 | 14.31 | - | 14.40 | 12.59 | 14.31
bench_vexsmp2 | 2 | 15.28 | - | 14.99 | 12.87 | 15.28
bench_vexsmp2 | 3 | - | 12.51 | - | 12.50 | -
bench_vexsmp2 | 4 | 14.81 | 13.34 | 14.85 | 12.92 | 14.81
bench_vexsmp2 | 5 | 13.58 | 14.49 | 14.16 | - | 13.58
bench_corescore25 | 1 | 25.92 | 25.45 | 25.39 | 23.52 | 25.92
bench_corescore25 | 2 | 29.43 | 25.41 | 25.62 | 24.04 | 29.43
bench_corescore25 | 3 | 26.81 | 25.04 | 25.68 | 22.80 | 26.81
bench_corescore25 | 4 | 25.52 | 25.61 | 24.80 | 25.78 | 25.52
bench_corescore25 | 5 | 24.51 | 24.56 | 22.70 | 21.92 | 24.51

## table3 vs table4 identity (all clocks)

- bench_vexsmp2 s3: not comparable (missing/unfinished)

## Missing / unfinished

- bench_vexsmp2 table4 s3 not finished (timeout)
- bench_vexsmp2 pm_rt s1 not finished (timeout)
- bench_vexsmp2 pm_rt s2 not finished (timeout)
- bench_vexsmp2 pt_rm s3 not finished (timeout)
- bench_vexsmp2 model4 s5 not finished (timeout)
- bench_vexsmp2 table3 s3 not finished (timeout)
