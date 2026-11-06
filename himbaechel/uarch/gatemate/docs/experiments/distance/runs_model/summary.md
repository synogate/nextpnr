# model vs base/both/linear (paired by seed; ratio = model / variant, primary = highest-constraint clock, last report)

design | variant | ok | primary clock | Fmax median | min-max | model/var geomean | wins (model) | sign p | route s | HeAP s | SA s | tables s | wall s
---|---|---|---|---|---|---|---|---|---|---|---|---|---
bench_dmg_soc | base | 8/8 | cc_bufg_inst_O | 28.38 | 26.20-29.98 | 1.006 (n=8) | 4-4 | 1.000 | 61 | 24 | 86 | 11.6 | 406
bench_dmg_soc | both | 8/8 | cc_bufg_inst_O | 28.38 | 26.20-29.98 | 1.006 (n=8) | 4-4 | 1.000 | 58 | 29 | 86 | 11.4 | 418
bench_dmg_soc | linear | 8/8 | cc_bufg_inst_O | 25.07 | 22.64-28.00 | 1.121 (n=8) | 7-1 | 0.070 | 139 | 20 | 32 | 20.8 | 1171
bench_dmg_soc | model | 8/8 | cc_bufg_inst_O | 28.20 | 27.14-29.72 |  |  |  | 55 | 60 | 115 | 13.9 | 1073
bench_corescore25 | base | 5/5 | clk | 29.20 | 27.73-29.68 | 0.840 (n=3) | 0-3 | 0.250 | 65 | 36 | 119 | 8.2 | 241
bench_corescore25 | both | 2/2 | clk | 28.70 | 27.73-29.68 | 0.845 (n=1) | 0-1 | 1.000 | 63 | 36 | 124 | 8.1 | 260
bench_corescore25 | linear | 5/5 | clk | 23.70 | 23.30-24.75 | 0.995 (n=3) | 1-2 | 1.000 | 93 | 28 | 82 | 9.0 | 251
bench_corescore25 | model | 3/5 | clk | 24.32 | 23.42-24.32 |  |  |  | 100 | 63 | 159 | 9.2 | 406
bench_mandelbrot | base | 5/5 | clk_pix | 68.07 | 67.09-74.38 | 0.882 (n=5) | 0-5 | 0.062 | 16 | 11 | 54 | 9.6 | 129
bench_mandelbrot | linear | 5/5 | clk_pix | 62.22 | 55.52-63.74 | 1.026 (n=5) | 3-2 | 1.000 | 60 | 21 | 29 | 9.8 | 187
bench_mandelbrot | model | 5/5 | clk_pix | 61.66 | 59.39-63.32 |  |  |  | 24 | 29 | 51 | 9.9 | 152
bench_minerva | base | 4/5 | main_crg_clkout | 22.05 | 21.82-22.60 | 0.722 (n=4) | 0-4 | 0.125 | 108 | 15 | 89 | 8.4 | 1790
bench_minerva | linear | 2/5 | main_crg_clkout | 21.38 | 21.19-21.57 | 0.756 (n=2) | 0-2 | 0.500 | 327 | 15 | 61 | 7.9 | 2848
bench_minerva | model | 5/5 | main_crg_clkout | 16.15 | 14.92-16.69 |  |  |  | 127 | 34 | 85 | 9.4 | 1965
bench_neorv32 | base | 5/5 | main_clkout | 27.05 | 26.97-27.15 | 0.724 (n=5) | 0-5 | 0.062 | 38 | 11 | 75 | 8.7 | 157
bench_neorv32 | linear | 5/5 | main_clkout | 24.88 | 24.03-25.66 | 0.788 (n=5) | 0-5 | 0.062 | 140 | 13 | 47 | 9.4 | 239
bench_neorv32 | model | 5/5 | main_clkout | 19.63 | 19.17-19.84 |  |  |  | 63 | 28 | 98 | 9.4 | 245
bench_picorv32 | base | 5/5 | main_clkout | 30.23 | 28.25-30.95 | 0.908 (n=5) | 0-5 | 0.062 | 63 | 23 | 79 | 9.3 | 266
bench_picorv32 | linear | 5/5 | main_clkout | 25.28 | 25.11-25.74 | 1.073 (n=5) | 5-0 | 0.062 | 215 | 12 | 92 | 9.3 | 371
bench_picorv32 | model | 5/5 | main_clkout | 27.13 | 26.37-28.10 |  |  |  | 82 | 25 | 50 | 10.5 | 203
bench_vexii | base | 5/5 | sys_clk | 27.90 | 27.02-29.02 | 0.862 (n=4) | 0-4 | 0.125 | 40 | 20 | 75 | 7.3 | 177
bench_vexii | linear | 5/5 | sys_clk | 24.15 | 24.02-24.44 | 0.998 (n=4) | 1-3 | 0.625 | 93 | 21 | 38 | 7.5 | 252
bench_vexii | model | 4/5 | sys_clk | 24.17 | 24.04-24.32 |  |  |  | 97 | 62 | 121 | 9.3 | 323
bench_vexriscv | base | 5/5 | VexRiscv.IBusCachedPlugin_cache.clk | 24.05 | 23.76-25.17 | 0.989 (n=5) | 2-3 | 1.000 | 82 | 21 | 73 | 9.8 | 238
bench_vexriscv | linear | 5/5 | VexRiscv.IBusCachedPlugin_cache.clk | 24.03 | 24.01-24.21 | 1.000 (n=5) | 3-2 | 1.000 | 137 | 12 | 36 | 9.3 | 384
bench_vexriscv | model | 5/5 | VexRiscv.IBusCachedPlugin_cache.clk | 24.04 | 24.01-24.23 |  |  |  | 106 | 34 | 69 | 10.9 | 293
bench_vexsmp2 | base | 5/5 | sys_clk | 14.34 | 13.49-15.39 | 0.962 (n=5) | 1-4 | 0.375 | 199 | 49 | 210 | 13.1 | 734
bench_vexsmp2 | both | 4/4 | sys_clk | 14.58 | 13.49-15.39 | 0.953 (n=4) | 1-3 | 0.625 | 202 | 48 | 206 | 12.8 | 771
bench_vexsmp2 | linear | 3/5 | sys_clk | 13.37 | 13.27-13.41 | 1.038 (n=3) | 2-1 | 1.000 | 344 | 59 | 170 | 21.1 | 2408
bench_vexsmp2 | model | 5/5 | sys_clk | 14.06 | 13.28-14.24 |  |  |  | 526 | 124 | 300 | 16.2 | 2719
multi1 | base | 12/12 | core[0].soc.clk | 46.15 | 42.68-48.28 | 0.801 (n=12) | 0-12 | 0.000 | 18 | 5 | 11 | 10.3 | 60
multi1 | linear | 12/12 | core[0].soc.clk | 37.74 | 35.13-38.99 | 0.982 (n=12) | 4-8 | 0.388 | 53 | 10 | 11 | 12.2 | 114
multi1 | model | 12/12 | core[0].soc.clk | 36.62 | 34.66-39.14 |  |  |  | 17 | 7 | 8 | 13.9 | 57

## All clocks (multi-clock designs)

design | variant | clock | Fmax median | min-max
---|---|---|---|---
bench_dmg_soc | base | cc_bufg_inst_O | 28.38 | 26.20-29.98
bench_dmg_soc | base | dmg0.dmg_clock | 40.22 | 36.94-41.95
bench_dmg_soc | base | dmgpp0.clk12m | 39.58 | 37.94-41.72
bench_dmg_soc | both | cc_bufg_inst_O | 28.38 | 26.20-29.98
bench_dmg_soc | both | dmg0.dmg_clock | 40.22 | 36.94-41.95
bench_dmg_soc | both | dmgpp0.clk12m | 39.58 | 37.94-41.72
bench_dmg_soc | linear | cc_bufg_inst_O | 25.07 | 22.64-28.00
bench_dmg_soc | linear | dmg0.dmg_clock | 33.75 | 31.62-36.06
bench_dmg_soc | linear | dmgpp0.clk12m | 33.87 | 31.10-36.43
bench_dmg_soc | model | cc_bufg_inst_O | 28.20 | 27.14-29.72
bench_dmg_soc | model | dmg0.dmg_clock | 34.84 | 33.54-41.13
bench_dmg_soc | model | dmgpp0.clk12m | 30.48 | 29.29-32.53
bench_mandelbrot | base | bitmap_addr_instance.clk | 39.76 | 39.28-41.83
bench_mandelbrot | base | clk_pix | 68.07 | 67.09-74.38
bench_mandelbrot | linear | bitmap_addr_instance.clk | 38.49 | 36.66-41.14
bench_mandelbrot | linear | clk_pix | 62.22 | 55.52-63.74
bench_mandelbrot | model | bitmap_addr_instance.clk | 29.47 | 29.18-30.25
bench_mandelbrot | model | clk_pix | 61.66 | 59.39-63.32

## Missing / unfinished

- bench_corescore25 both s3 missing
- bench_corescore25 both s4 missing
- bench_corescore25 both s5 missing
- bench_corescore25 model s1 not finished (timeout)
- bench_corescore25 model s3 not finished (timeout)
- bench_minerva base s2 not finished (timeout)
- bench_minerva linear s3 not finished (timeout)
- bench_minerva linear s4 not finished (timeout)
- bench_minerva linear s5 not finished (timeout)
- bench_vexii model s1 not finished (timeout)
- bench_vexsmp2 both s5 missing
- bench_vexsmp2 linear s1 not finished (timeout)
- bench_vexsmp2 linear s2 not finished (timeout)
