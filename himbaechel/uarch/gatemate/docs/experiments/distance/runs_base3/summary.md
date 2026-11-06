# Distance model baseline redo (`nextpnr-himbaechel.distance_model3.exe`, main-based)

Same binary for linear3/table3/model3. table3 = `--vopt lookahead=table --vopt place-delay=table`: builds the delay tables and uses them in router, placer and post-route repairs (= old main default-with-tables flow). linear3 = `lookahead=linear place-delay=linear`: linear router/placer; NOTE: the repairs use the closed-form model, not the tables (the old `linear` flow used the tables in the repairs). model3 (runs_model3/) = `lookahead=model place-delay=model`. Secondary, from runs_model2/ (earlier main build, first model version): model2 = model everywhere, base2 = no option (linear router/placer, model repairs). The earlier runs/ baseline (linear/base/both) was built from the planemate branch and is not comparable.

Fmax: last report; dmg_soc primary clock cc_bufg_inst_O (33 MHz PLL), others the highest-constraint clock. Ratio = per-seed Fmax a/b, geometric mean; ties counted apart, two-sided sign test on wins/losses.

## bench_dmg_soc per seed (cc_bufg_inst_O, MHz)

seed | linear3 | table3 | model3 | model2 | base2 | route s lin/tab/mod3 | wall s lin/tab/mod3 | kept rep lin/tab/mod3
---|---|---|---|---|---|---|---|---|
1 | 26.84 | 29.67 | 27.61 | 28.45 | 27.36 | 119.3/58.5/68.6 | 2652.9/2267.5/765.0 | 34/12/23
2 | 28.27 | 28.41 | 26.09 | 28.81 | 27.43 | 109.7/116.9/81.1 | 1332.3/647.6/801.1 | 50/11/20
3 | 26.22 | 27.47 | 27.73 | 27.67 | 27.71 | 121.9/44.0/74.3 | 912.8/701.6/556.9 | 22/11/5
4 | 23.30 | 26.33 | 27.59 | 26.67 | 23.30 | 154.8/78.7/64.9 | 2193.8/703.2/652.9 | 20/12/12
5 | 24.33 | 28.19 | 27.73 | 26.98 | 24.33 | 109.7/112.8/77.7 | 2853.3/618.3/1289.1 | 8/1/14
6 | 26.46 | 29.14 | 27.05 | 25.89 | 27.30 | 137.8/76.8/66.0 | 1030.3/1010.0/734.1 | 29/25/14
7 | 24.00 | 29.47 | 26.04 | 29.70 | 24.01 | 190.4/2657.7/63.0 | 981.2/3139.8/2203.3 | 24/17/8
8 | 27.57 | 29.87 | 29.23 | 28.57 | 25.29 | 101.8/54.6/59.5 | 1022.2/539.3/675.4 | 48/11/15

## Paired ratios

design | ratio | n | geomean | wins-losses-ties | sign p
---|---|---|---|---|---
bench_dmg_soc | model3/table3 | 8 | 0.959 | 2-6-0 | 0.289
bench_dmg_soc | model3/linear3 | 8 | 1.060 | 7-1-0 | 0.070
bench_dmg_soc | table3/linear3 | 8 | 1.106 | 8-0-0 | 0.008
bench_dmg_soc | model2/table3 | 8 | 0.974 | 4-4-0 | 1.000
bench_dmg_soc | base2/linear3 | 8 | 0.999 | 4-2-2 | 0.688
bench_corescore25 | model3/table3 | 3 | 0.891 | 0-3-0 | 0.250
bench_corescore25 | model3/linear3 | 3 | 1.099 | 3-0-0 | 0.250
bench_corescore25 | table3/linear3 | 5 | 1.222 | 5-0-0 | 0.062
bench_corescore25 | model2/table3 | 3 | 0.910 | 0-3-0 | 0.250
bench_corescore25 | base2/linear3 | 5 | 1.000 | 0-0-5 | nan
bench_mandelbrot | model3/table3 | 5 | 0.961 | 1-4-0 | 0.375
bench_mandelbrot | model3/linear3 | 5 | 1.090 | 4-1-0 | 0.375
bench_mandelbrot | table3/linear3 | 5 | 1.134 | 4-1-0 | 0.375
bench_mandelbrot | model2/table3 | 5 | 0.958 | 2-3-0 | 1.000
bench_mandelbrot | base2/linear3 | 5 | 1.000 | 0-0-5 | nan
bench_minerva | model3/table3 | 5 | 1.006 | 3-2-0 | 1.000
bench_minerva | model3/linear3 | 1 | 1.117 | 1-0-0 | 1.000
bench_minerva | table3/linear3 | 1 | 1.122 | 1-0-0 | 1.000
bench_minerva | model2/table3 | 5 | 0.993 | 2-3-0 | 1.000
bench_neorv32 | model3/table3 | 5 | 1.005 | 4-0-1 | 0.125
bench_neorv32 | model3/linear3 | 5 | 1.010 | 3-2-0 | 1.000
bench_neorv32 | table3/linear3 | 5 | 1.004 | 1-4-0 | 0.375
bench_neorv32 | model2/table3 | 5 | 1.006 | 4-1-0 | 0.375
bench_neorv32 | base2/linear3 | 5 | 1.000 | 0-0-5 | nan
bench_picorv32 | model3/table3 | 5 | 0.996 | 2-3-0 | 1.000
bench_picorv32 | model3/linear3 | 5 | 1.107 | 5-0-0 | 0.062
bench_picorv32 | table3/linear3 | 5 | 1.111 | 5-0-0 | 0.062
bench_picorv32 | model2/table3 | 5 | 0.990 | 2-3-0 | 1.000
bench_picorv32 | base2/linear3 | 5 | 1.000 | 1-0-4 | 1.000
bench_vexii | model3/table3 | 5 | 0.959 | 0-5-0 | 0.062
bench_vexii | model3/linear3 | 5 | 1.040 | 5-0-0 | 0.062
bench_vexii | table3/linear3 | 5 | 1.085 | 5-0-0 | 0.062
bench_vexii | model2/table3 | 4 | 0.911 | 0-4-0 | 0.125
bench_vexii | base2/linear3 | 5 | 1.000 | 1-2-2 | 1.000
bench_vexriscv | model3/table3 | 5 | 0.998 | 3-1-1 | 0.625
bench_vexriscv | model3/linear3 | 4 | 1.018 | 3-0-1 | 0.250
bench_vexriscv | table3/linear3 | 4 | 1.021 | 3-1-0 | 0.625
bench_vexriscv | model2/table3 | 5 | 0.996 | 1-4-0 | 0.375
bench_vexriscv | base2/linear3 | 2 | 1.019 | 1-1-0 | 1.000
bench_vexsmp2 | model3/table3 | 3 | 0.856 | 0-3-0 | 0.250
bench_vexsmp2 | model2/table3 | 4 | 0.878 | 0-4-0 | 0.125
multi1 | model3/table3 | 12 | 0.911 | 1-11-0 | 0.006
multi1 | model3/linear3 | 12 | 1.110 | 12-0-0 | 0.000
multi1 | table3/linear3 | 12 | 1.218 | 12-0-0 | 0.000
multi1 | model2/table3 | 12 | 0.903 | 0-12-0 | 0.000
multi1 | base2/linear3 | 12 | 1.000 | 0-0-12 | nan

## Per variant (medians over finished runs)

design | variant | ok | Fmax median | min-max | route s | place s | tables s | wall s | timeouts | kept rep/moves/fused (median)
---|---|---|---|---|---|---|---|---|---|---
bench_dmg_soc | linear3 | 8/8 | 26.34 | 23.30-28.27 | 120.6 | 142.0 | none | 1181.3 | 0 | 26.5/13/0
bench_dmg_soc | table3 | 8/8 | 28.77 | 26.33-29.87 | 77.7 | 204.6 | 10.8 | 702.4 | 0 | 11.5/8/0
bench_dmg_soc | model3 | 8/8 | 27.60 | 26.04-29.23 | 67.3 | 209.0 | none | 749.5 | 0 | 14/8/0.5
bench_dmg_soc | model2 | 8/8 | 28.06 | 25.89-29.70 | 59.3 | 160.8 | none | 655.2 | 0 | 15.5/7/0
bench_dmg_soc | base2 | 8/8 | 26.30 | 23.30-27.71 | 116.1 | 115.2 | none | 1105.3 | 0 | 26/12.5/0
bench_corescore25 | linear3 | 5/5 | 21.34 | 20.48-23.34 | 132.1 | 163.1 | none | 342.9 | 0 | 0/0/0
bench_corescore25 | table3 | 5/5 | 25.92 | 24.51-29.43 | 82.7 | 173.4 | 8.9 | 340.1 | 0 | 0/0/0
bench_corescore25 | model3 | 3/5 | 22.99 | 22.93-23.76 | 131.0 | 184.0 | none | 355.1 | 2 | 0/0/0
bench_corescore25 | model2 | 3/5 | 24.32 | 23.42-24.32 | 109.7 | 202.2 | none | 353.2 | 2 | 0/0/0
bench_corescore25 | base2 | 5/5 | 21.34 | 20.48-23.34 | 175.2 | 223.1 | none | 452.4 | 0 | 0/0/0
bench_mandelbrot | linear3 | 5/5 | 53.61 | 51.60-65.69 | 19.1 | 32.4 | none | 76.2 | 0 | 0/0/0
bench_mandelbrot | table3 | 5/5 | 65.04 | 55.81-72.38 | 10.4 | 46.1 | 7.0 | 91.6 | 0 | 0/0/0
bench_mandelbrot | model3 | 5/5 | 63.04 | 54.07-65.58 | 18.0 | 68.6 | none | 128.2 | 0 | 0/0/0
bench_mandelbrot | model2 | 5/5 | 61.66 | 59.39-63.32 | 21.0 | 71.6 | none | 129.1 | 0 | 0/0/0
bench_mandelbrot | base2 | 5/5 | 53.61 | 51.60-65.69 | 30.5 | 50.3 | none | 119.8 | 0 | 0/0/0
bench_minerva | linear3 | 1/5 | 14.55 | 14.55-14.55 | 237.7 | 79.0 | none | 3419.6 | 4 | 1/27/0
bench_minerva | table3 | 5/5 | 16.68 | 16.33-16.83 | 77.9 | 96.6 | 7.6 | 516.9 | 0 | 2/8/0
bench_minerva | model3 | 5/5 | 16.78 | 16.25-17.10 | 109.4 | 128.6 | none | 1699.6 | 0 | 3/12/0
bench_minerva | model2 | 5/5 | 16.40 | 16.18-17.02 | 85.7 | 85.1 | none | 1745.6 | 0 | 4/17/0
bench_minerva | base2 | 0/5 | nan |  | - | - | none | - | 5 | 
bench_neorv32 | linear3 | 5/5 | 19.47 | 18.69-19.66 | 96.0 | 56.2 | none | 186.7 | 0 | 1/0/0
bench_neorv32 | table3 | 5/5 | 19.51 | 19.20-19.64 | 38.1 | 80.9 | 8.8 | 151.5 | 0 | 0/0/0
bench_neorv32 | model3 | 5/5 | 19.54 | 19.51-19.66 | 50.0 | 101.4 | none | 188.5 | 0 | 0/0/0
bench_neorv32 | model2 | 5/5 | 19.59 | 19.17-19.84 | 63.1 | 126.9 | none | 225.2 | 0 | 0/0/0
bench_neorv32 | base2 | 5/5 | 19.47 | 18.69-19.66 | 114.0 | 62.9 | none | 216.9 | 0 | 1/0/0
bench_picorv32 | linear3 | 5/5 | 24.68 | 24.20-25.60 | 105.2 | 39.0 | none | 163.0 | 0 | 3/0/0
bench_picorv32 | table3 | 5/5 | 27.37 | 27.14-28.09 | 38.8 | 45.1 | 7.5 | 108.0 | 0 | 0/0/0
bench_picorv32 | model3 | 5/5 | 26.80 | 25.52-29.38 | 68.8 | 81.3 | none | 175.2 | 0 | 0/0/0
bench_picorv32 | model2 | 5/5 | 27.13 | 26.37-28.10 | 73.7 | 67.6 | none | 175.0 | 0 | 0/0/0
bench_picorv32 | base2 | 5/5 | 24.68 | 24.21-25.60 | 138.8 | 48.9 | none | 214.7 | 0 | 3/0/0
bench_vexii | linear3 | 5/5 | 24.04 | 24.01-24.06 | 118.4 | 123.9 | none | 377.1 | 0 | 16/0/0
bench_vexii | table3 | 5/5 | 26.63 | 24.12-27.45 | 49.8 | 125.3 | 9.4 | 210.9 | 0 | 0/0/0
bench_vexii | model3 | 5/5 | 24.56 | 24.09-26.51 | 73.2 | 140.4 | none | 267.4 | 0 | 2/0/0
bench_vexii | model2 | 4/5 | 24.21 | 24.04-24.38 | 70.3 | 150.1 | none | 254.8 | 1 | 0/0/0
bench_vexii | base2 | 5/5 | 24.03 | 24.01-24.05 | 157.7 | 156.7 | none | 536.7 | 0 | 16/0/0
bench_vexriscv | linear3 | 4/5 | 23.76 | 23.01-24.12 | 170.9 | 52.8 | none | 2428.0 | 1 | 32.5/74/0
bench_vexriscv | table3 | 5/5 | 24.08 | 24.00-24.43 | 49.6 | 80.5 | 10.3 | 184.0 | 0 | 6/0/0
bench_vexriscv | model3 | 5/5 | 24.12 | 24.01-24.18 | 96.3 | 98.7 | none | 562.2 | 0 | 10/16/0
bench_vexriscv | model2 | 5/5 | 24.08 | 23.95-24.12 | 69.8 | 97.3 | none | 498.0 | 0 | 5/6/0
bench_vexriscv | base2 | 3/5 | 23.93 | 23.71-24.01 | 158.4 | 61.9 | none | 2223.7 | 2 | 79/84/0
bench_vexsmp2 | linear3 | 0/5 | nan |  | - | - | none | - | 5 | 
bench_vexsmp2 | table3 | 4/5 | 14.56 | 13.58-15.28 | 431.5 | 338.4 | 10.2 | 1846.0 | 1 | 1.5/4.5/0
bench_vexsmp2 | model3 | 4/5 | 12.81 | 12.38-13.06 | 377.5 | 418.8 | none | 1115.2 | 1 | 5.5/1/0
bench_vexsmp2 | model2 | 5/5 | 13.02 | 11.98-13.56 | 425.3 | 409.2 | none | 1562.9 | 0 | 4/4/0
bench_vexsmp2 | base2 | 0/5 | nan |  | - | - | none | - | 5 | 
multi1 | linear3 | 12/12 | 32.98 | 28.35-35.69 | 23.1 | 10.8 | none | 44.0 | 0 | 0/0/0
multi1 | table3 | 12/12 | 40.48 | 37.34-43.69 | 10.9 | 10.5 | 6.4 | 37.1 | 0 | 0/0/0
multi1 | model3 | 12/12 | 37.42 | 32.38-39.08 | 20.5 | 15.1 | none | 49.8 | 0 | 0/0/0
multi1 | model2 | 12/12 | 36.62 | 34.66-39.14 | 27.0 | 23.7 | none | 62.3 | 0 | 0/0/0
multi1 | base2 | 12/12 | 32.98 | 28.35-35.69 | 58.1 | 31.7 | none | 105.3 | 0 | 0/0/0

## All clocks (multi-clock designs)

design | variant | clock | Fmax median | min-max
---|---|---|---|---
bench_dmg_soc | linear3 | cc_bufg_inst_O | 26.34 | 23.30-28.27
bench_dmg_soc | linear3 | dmg0.dmg_clock | 29.29 | 26.85-36.79
bench_dmg_soc | linear3 | dmgpp0.clk12m | 29.60 | 27.93-30.81
bench_dmg_soc | table3 | cc_bufg_inst_O | 28.77 | 26.33-29.87
bench_dmg_soc | table3 | dmg0.dmg_clock | 39.64 | 36.83-42.61
bench_dmg_soc | table3 | dmgpp0.clk12m | 32.01 | 30.49-33.83
bench_dmg_soc | model3 | cc_bufg_inst_O | 27.60 | 26.04-29.23
bench_dmg_soc | model3 | dmg0.dmg_clock | 35.43 | 29.56-37.32
bench_dmg_soc | model3 | dmgpp0.clk12m | 31.63 | 29.47-34.23
bench_dmg_soc | model2 | cc_bufg_inst_O | 28.06 | 25.89-29.70
bench_dmg_soc | model2 | dmg0.dmg_clock | 34.84 | 33.54-41.13
bench_dmg_soc | model2 | dmgpp0.clk12m | 30.48 | 29.53-32.44
bench_dmg_soc | base2 | cc_bufg_inst_O | 26.30 | 23.30-27.71
bench_dmg_soc | base2 | dmg0.dmg_clock | 29.29 | 27.35-36.79
bench_dmg_soc | base2 | dmgpp0.clk12m | 29.60 | 27.93-30.81
bench_mandelbrot | linear3 | bitmap_addr_instance.clk | 27.68 | 25.72-30.10
bench_mandelbrot | linear3 | clk_pix | 53.61 | 51.60-65.69
bench_mandelbrot | table3 | bitmap_addr_instance.clk | 30.12 | 29.22-30.25
bench_mandelbrot | table3 | clk_pix | 65.04 | 55.81-72.38
bench_mandelbrot | model3 | bitmap_addr_instance.clk | 29.55 | 29.17-29.96
bench_mandelbrot | model3 | clk_pix | 63.04 | 54.07-65.58
bench_mandelbrot | model2 | bitmap_addr_instance.clk | 29.47 | 29.18-30.25
bench_mandelbrot | model2 | clk_pix | 61.66 | 59.39-63.32
bench_mandelbrot | base2 | bitmap_addr_instance.clk | 27.68 | 25.72-30.10
bench_mandelbrot | base2 | clk_pix | 53.61 | 51.60-65.69

## Missing / unfinished

- bench_corescore25 model3 s2 not finished (timeout)
- bench_corescore25 model3 s5 not finished (timeout)
- bench_corescore25 model2 s1 not finished (timeout)
- bench_corescore25 model2 s3 not finished (timeout)
- bench_minerva linear3 s2 not finished (timeout)
- bench_minerva linear3 s3 not finished (timeout)
- bench_minerva linear3 s4 not finished (timeout)
- bench_minerva linear3 s5 not finished (timeout)
- bench_minerva base2 s1 not finished (timeout)
- bench_minerva base2 s2 not finished (timeout)
- bench_minerva base2 s3 not finished (timeout)
- bench_minerva base2 s4 not finished (timeout)
- bench_minerva base2 s5 not finished (timeout)
- bench_vexii model2 s1 not finished (timeout)
- bench_vexriscv linear3 s3 not finished (timeout)
- bench_vexriscv base2 s2 not finished (timeout)
- bench_vexriscv base2 s5 not finished (timeout)
- bench_vexsmp2 linear3 s1 not finished (timeout)
- bench_vexsmp2 linear3 s2 not finished (timeout)
- bench_vexsmp2 linear3 s3 not finished (timeout)
- bench_vexsmp2 linear3 s4 not finished (timeout)
- bench_vexsmp2 linear3 s5 not finished (timeout)
- bench_vexsmp2 table3 s3 not finished (timeout)
- bench_vexsmp2 model3 s5 not finished (timeout)
- bench_vexsmp2 base2 s1 not finished (timeout)
- bench_vexsmp2 base2 s2 not finished (timeout)
- bench_vexsmp2 base2 s3 not finished (timeout)
- bench_vexsmp2 base2 s4 not finished (timeout)
- bench_vexsmp2 base2 s5 not finished (timeout)
