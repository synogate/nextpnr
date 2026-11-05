# Distance model step 2 (`nextpnr-himbaechel.distance_model2.exe`): model2 / base2 vs model, base, both, linear

model2 = lookahead=model place-delay=model, no tables at all (repairs + LUT swap use the model); base2 = no option (router/placer linear, repairs use the model). From runs/: base = old default (builds and uses the tables, identical to both), both = tables, linear = explicit linear (tables only for repairs). runs_model/: model = model router/placer, tables for repairs. Fmax: last report; dmg_soc primary clock cc_bufg_inst_O (33 MHz PLL), others the highest-constraint clock. Ratio = per-seed Fmax a/b, geometric mean; ties counted apart, sign test on wins/losses.

## Paired ratios

design | ratio | n | geomean | wins-losses-ties | sign p
---|---|---|---|---|---
bench_dmg_soc | model2/both | 8 | 0.992 | 3-5-0 | 0.727
bench_dmg_soc | model2/model | 8 | 0.986 | 1-6-1 | 0.125
bench_dmg_soc | model2/base | 8 | 0.992 | 3-5-0 | 0.727
bench_dmg_soc | model2/linear | 8 | 1.106 | 7-1-0 | 0.070
bench_dmg_soc | base2/base | 8 | 0.919 | 3-5-0 | 0.727
bench_dmg_soc | base2/linear | 8 | 1.025 | 4-4-0 | 1.000
bench_corescore25 | model2/both | 1 | 0.845 | 0-1-0 | 1.000
bench_corescore25 | model2/model | 3 | 1.000 | 0-0-3 | nan
bench_corescore25 | model2/base | 3 | 0.840 | 0-3-0 | 0.250
bench_corescore25 | model2/linear | 3 | 0.995 | 1-2-0 | 1.000
bench_corescore25 | base2/base | 5 | 0.745 | 0-5-0 | 0.062
bench_corescore25 | base2/linear | 5 | 0.906 | 0-5-0 | 0.062
bench_mandelbrot | model2/model | 5 | 1.000 | 0-0-5 | nan
bench_mandelbrot | model2/base | 5 | 0.882 | 0-5-0 | 0.062
bench_mandelbrot | model2/linear | 5 | 1.026 | 3-2-0 | 1.000
bench_mandelbrot | base2/base | 5 | 0.811 | 0-5-0 | 0.062
bench_mandelbrot | base2/linear | 5 | 0.943 | 1-4-0 | 0.375
bench_minerva | model2/model | 5 | 1.032 | 5-0-0 | 0.062
bench_minerva | model2/base | 4 | 0.748 | 0-4-0 | 0.125
bench_minerva | model2/linear | 2 | 0.767 | 0-2-0 | 0.500
bench_neorv32 | model2/model | 5 | 1.000 | 0-1-4 | 1.000
bench_neorv32 | model2/base | 5 | 0.723 | 0-5-0 | 0.062
bench_neorv32 | model2/linear | 5 | 0.788 | 0-5-0 | 0.062
bench_neorv32 | base2/base | 5 | 0.716 | 0-5-0 | 0.062
bench_neorv32 | base2/linear | 5 | 0.780 | 0-5-0 | 0.062
bench_picorv32 | model2/model | 5 | 1.000 | 0-0-5 | nan
bench_picorv32 | model2/base | 5 | 0.908 | 0-5-0 | 0.062
bench_picorv32 | model2/linear | 5 | 1.073 | 5-0-0 | 0.062
bench_picorv32 | base2/base | 5 | 0.826 | 0-5-0 | 0.062
bench_picorv32 | base2/linear | 5 | 0.975 | 0-5-0 | 0.062
bench_vexii | model2/model | 4 | 1.001 | 1-0-3 | 1.000
bench_vexii | model2/base | 4 | 0.863 | 0-4-0 | 0.125
bench_vexii | model2/linear | 4 | 1.000 | 1-3-0 | 0.625
bench_vexii | base2/base | 5 | 0.857 | 0-5-0 | 0.062
bench_vexii | base2/linear | 5 | 0.994 | 1-4-0 | 0.375
bench_vexriscv | model2/model | 5 | 0.999 | 2-2-1 | 1.000
bench_vexriscv | model2/base | 5 | 0.988 | 3-2-0 | 1.000
bench_vexriscv | model2/linear | 5 | 1.000 | 3-2-0 | 1.000
bench_vexriscv | base2/base | 3 | 0.997 | 0-3-0 | 0.250
bench_vexriscv | base2/linear | 3 | 0.994 | 0-2-1 | 0.500
bench_vexsmp2 | model2/both | 4 | 0.875 | 0-4-0 | 0.125
bench_vexsmp2 | model2/model | 5 | 0.919 | 0-5-0 | 0.062
bench_vexsmp2 | model2/base | 5 | 0.884 | 0-5-0 | 0.062
bench_vexsmp2 | model2/linear | 3 | 0.952 | 0-3-0 | 0.250
multi1 | model2/model | 12 | 1.000 | 0-0-12 | nan
multi1 | model2/base | 12 | 0.801 | 0-12-0 | 0.000
multi1 | model2/linear | 12 | 0.982 | 4-8-0 | 0.388
multi1 | base2/base | 12 | 0.728 | 0-12-0 | 0.000
multi1 | base2/linear | 12 | 0.892 | 1-11-0 | 0.006

## Per variant (medians over finished runs)

design | variant | ok | Fmax median | min-max | route s | place s | tables s | wall s | kept rep/moves/fused (median)
---|---|---|---|---|---|---|---|---|---
bench_dmg_soc | model2 | 8/8 | 28.06 | 25.89-29.70 | 59.3 | 160.8 | none | 655.2 | 15.5/7/0
bench_dmg_soc | base2 | 8/8 | 26.30 | 23.30-27.71 | 116.1 | 115.2 | none | 1105.3 | 26/12.5/0
bench_dmg_soc | model | 8/8 | 28.20 | 27.14-29.72 | 54.5 | 168.5 | 13.9 | 1073.5 | 21/7/0
bench_dmg_soc | base | 8/8 | 28.38 | 26.20-29.98 | 61.2 | 113.6 | 11.6 | 405.6 | 8.5/8/0
bench_dmg_soc | both | 8/8 | 28.38 | 26.20-29.98 | 57.7 | 113.4 | 11.4 | 417.6 | 8.5/8/0
bench_dmg_soc | linear | 8/8 | 25.07 | 22.64-28.00 | 139.4 | 51.6 | 20.8 | 1170.9 | 30.5/11.5/0
bench_corescore25 | model2 | 3/5 | 24.32 | 23.42-24.32 | 109.7 | 202.2 | none | 353.2 | 0/0/0
bench_corescore25 | base2 | 5/5 | 21.34 | 20.48-23.34 | 175.2 | 223.1 | none | 452.4 | 0/0/0
bench_corescore25 | model | 3/5 | 24.32 | 23.42-24.32 | 99.6 | 247.1 | 9.2 | 406.1 | 0/0/0
bench_corescore25 | base | 5/5 | 29.20 | 27.73-29.68 | 65.5 | 157.8 | 8.2 | 241.0 | 0/0/0
bench_corescore25 | both | 2/5 | 28.70 | 27.73-29.68 | 62.8 | 160.0 | 8.1 | 260.0 | 0/0/0
bench_corescore25 | linear | 5/5 | 23.70 | 23.30-24.75 | 92.9 | 107.3 | 9.0 | 250.8 | 0/0/0
bench_mandelbrot | model2 | 5/5 | 61.66 | 59.39-63.32 | 21.0 | 71.6 | none | 129.1 | 0/0/0
bench_mandelbrot | base2 | 5/5 | 53.61 | 51.60-65.69 | 30.5 | 50.3 | none | 119.8 | 0/0/0
bench_mandelbrot | model | 5/5 | 61.66 | 59.39-63.32 | 23.7 | 79.5 | 9.9 | 152.2 | 0/0/0
bench_mandelbrot | base | 5/5 | 68.07 | 67.09-74.38 | 16.1 | 65.0 | 9.6 | 129.3 | 0/0/0
bench_mandelbrot | linear | 5/5 | 62.22 | 55.52-63.74 | 60.0 | 50.3 | 9.8 | 186.6 | 0/0/0
bench_minerva | model2 | 5/5 | 16.40 | 16.18-17.02 | 85.7 | 85.1 | none | 1745.6 | 4/17/0
bench_minerva | base2 | 0/5 | nan |  | - | - | none | - | 
bench_minerva | model | 5/5 | 16.15 | 14.92-16.69 | 126.8 | 111.4 | 9.4 | 1965.4 | 3/11/0
bench_minerva | base | 4/5 | 22.05 | 21.82-22.60 | 108.5 | 99.7 | 8.4 | 1790.2 | 6.5/39/0
bench_minerva | linear | 2/5 | 21.38 | 21.19-21.57 | 326.8 | 75.4 | 7.9 | 2848.1 | 24.5/46.5/0
bench_neorv32 | model2 | 5/5 | 19.59 | 19.17-19.84 | 63.1 | 126.9 | none | 225.2 | 0/0/0
bench_neorv32 | base2 | 5/5 | 19.47 | 18.69-19.66 | 114.0 | 62.9 | none | 216.9 | 1/0/0
bench_neorv32 | model | 5/5 | 19.63 | 19.17-19.84 | 63.0 | 125.7 | 9.4 | 244.8 | 0/0/0
bench_neorv32 | base | 5/5 | 27.05 | 26.97-27.15 | 37.6 | 86.1 | 8.7 | 156.6 | 0/0/0
bench_neorv32 | linear | 5/5 | 24.88 | 24.03-25.66 | 139.6 | 59.5 | 9.4 | 239.1 | 0/0/0
bench_picorv32 | model2 | 5/5 | 27.13 | 26.37-28.10 | 73.7 | 67.6 | none | 175.0 | 0/0/0
bench_picorv32 | base2 | 5/5 | 24.68 | 24.21-25.60 | 138.8 | 48.9 | none | 214.7 | 3/0/0
bench_picorv32 | model | 5/5 | 27.13 | 26.37-28.10 | 82.2 | 85.0 | 10.5 | 202.9 | 0/0/0
bench_picorv32 | base | 5/5 | 30.23 | 28.25-30.95 | 62.8 | 102.1 | 9.3 | 265.5 | 0/0/0
bench_picorv32 | linear | 5/5 | 25.28 | 25.11-25.74 | 215.3 | 102.2 | 9.3 | 371.1 | 0/0/0
bench_vexii | model2 | 4/5 | 24.21 | 24.04-24.38 | 70.3 | 150.1 | none | 254.8 | 0/0/0
bench_vexii | base2 | 5/5 | 24.03 | 24.01-24.05 | 157.7 | 156.7 | none | 536.7 | 16/0/0
bench_vexii | model | 4/5 | 24.17 | 24.04-24.32 | 97.1 | 176.1 | 9.3 | 323.4 | 0/0/0
bench_vexii | base | 5/5 | 27.90 | 27.02-29.02 | 39.8 | 94.8 | 7.3 | 177.4 | 0/0/0
bench_vexii | linear | 5/5 | 24.15 | 24.02-24.44 | 93.3 | 56.4 | 7.5 | 252.2 | 9/2/0
bench_vexriscv | model2 | 5/5 | 24.08 | 23.95-24.12 | 69.8 | 97.3 | none | 498.0 | 5/6/0
bench_vexriscv | base2 | 3/5 | 23.93 | 23.71-24.01 | 158.4 | 61.9 | none | 2223.7 | 79/84/0
bench_vexriscv | model | 5/5 | 24.04 | 24.01-24.23 | 106.1 | 97.6 | 10.9 | 292.6 | 8/3/0
bench_vexriscv | base | 5/5 | 24.05 | 23.76-25.17 | 82.1 | 96.3 | 9.8 | 238.0 | 0/0/0
bench_vexriscv | linear | 5/5 | 24.03 | 24.01-24.21 | 137.1 | 44.4 | 9.3 | 383.7 | 24/6/0
bench_vexsmp2 | model2 | 5/5 | 13.02 | 11.98-13.56 | 425.3 | 409.2 | none | 1562.9 | 4/4/0
bench_vexsmp2 | base2 | 0/5 | nan |  | - | - | none | - | 
bench_vexsmp2 | model | 5/5 | 14.06 | 13.28-14.24 | 525.7 | 444.2 | 16.2 | 2718.6 | 9/17/0
bench_vexsmp2 | base | 5/5 | 14.34 | 13.49-15.39 | 199.0 | 259.5 | 13.1 | 734.1 | 4/9/0
bench_vexsmp2 | both | 4/5 | 14.58 | 13.49-15.39 | 201.8 | 257.0 | 12.8 | 771.2 | 3.5/10/0
bench_vexsmp2 | linear | 3/5 | 13.37 | 13.27-13.41 | 344.4 | 228.1 | 21.1 | 2408.2 | 23/38/0
multi1 | model2 | 12/12 | 36.62 | 34.66-39.14 | 27.0 | 23.7 | none | 62.3 | 0/0/0
multi1 | base2 | 12/12 | 32.98 | 28.35-35.69 | 58.1 | 31.7 | none | 105.3 | 0/0/0
multi1 | model | 12/12 | 36.62 | 34.66-39.14 | 17.1 | 15.2 | 13.9 | 57.2 | 0/0/0
multi1 | base | 12/12 | 46.15 | 42.68-48.28 | 17.6 | 16.7 | 10.3 | 59.8 | 0/0/0
multi1 | linear | 12/12 | 37.74 | 35.13-38.99 | 52.7 | 22.9 | 12.2 | 113.8 | 0/0/0

## All clocks (multi-clock designs)

design | variant | clock | Fmax median | min-max
---|---|---|---|---
bench_dmg_soc | model2 | cc_bufg_inst_O | 28.06 | 25.89-29.70
bench_dmg_soc | model2 | dmg0.dmg_clock | 34.84 | 33.54-41.13
bench_dmg_soc | model2 | dmgpp0.clk12m | 30.48 | 29.53-32.44
bench_dmg_soc | base2 | cc_bufg_inst_O | 26.30 | 23.30-27.71
bench_dmg_soc | base2 | dmg0.dmg_clock | 29.29 | 27.35-36.79
bench_dmg_soc | base2 | dmgpp0.clk12m | 29.60 | 27.93-30.81
bench_dmg_soc | model | cc_bufg_inst_O | 28.20 | 27.14-29.72
bench_dmg_soc | model | dmg0.dmg_clock | 34.84 | 33.54-41.13
bench_dmg_soc | model | dmgpp0.clk12m | 30.48 | 29.29-32.53
bench_dmg_soc | base | cc_bufg_inst_O | 28.38 | 26.20-29.98
bench_dmg_soc | base | dmg0.dmg_clock | 40.22 | 36.94-41.95
bench_dmg_soc | base | dmgpp0.clk12m | 39.58 | 37.94-41.72
bench_dmg_soc | both | cc_bufg_inst_O | 28.38 | 26.20-29.98
bench_dmg_soc | both | dmg0.dmg_clock | 40.22 | 36.94-41.95
bench_dmg_soc | both | dmgpp0.clk12m | 39.58 | 37.94-41.72
bench_dmg_soc | linear | cc_bufg_inst_O | 25.07 | 22.64-28.00
bench_dmg_soc | linear | dmg0.dmg_clock | 33.75 | 31.62-36.06
bench_dmg_soc | linear | dmgpp0.clk12m | 33.87 | 31.10-36.43
bench_mandelbrot | model2 | bitmap_addr_instance.clk | 29.47 | 29.18-30.25
bench_mandelbrot | model2 | clk_pix | 61.66 | 59.39-63.32
bench_mandelbrot | base2 | bitmap_addr_instance.clk | 27.68 | 25.72-30.10
bench_mandelbrot | base2 | clk_pix | 53.61 | 51.60-65.69
bench_mandelbrot | model | bitmap_addr_instance.clk | 29.47 | 29.18-30.25
bench_mandelbrot | model | clk_pix | 61.66 | 59.39-63.32
bench_mandelbrot | base | bitmap_addr_instance.clk | 39.76 | 39.28-41.83
bench_mandelbrot | base | clk_pix | 68.07 | 67.09-74.38
bench_mandelbrot | linear | bitmap_addr_instance.clk | 38.49 | 36.66-41.14
bench_mandelbrot | linear | clk_pix | 62.22 | 55.52-63.74

## Missing / unfinished

- bench_corescore25 model2 s1 not finished (timeout)
- bench_corescore25 model2 s3 not finished (timeout)
- bench_corescore25 model s1 not finished (timeout)
- bench_corescore25 model s3 not finished (timeout)
- bench_corescore25 both s3 missing
- bench_corescore25 both s4 missing
- bench_corescore25 both s5 missing
- bench_mandelbrot both s1 missing
- bench_mandelbrot both s2 missing
- bench_mandelbrot both s3 missing
- bench_mandelbrot both s4 missing
- bench_mandelbrot both s5 missing
- bench_minerva base2 s1 not finished (timeout)
- bench_minerva base2 s2 not finished (timeout)
- bench_minerva base2 s3 not finished (timeout)
- bench_minerva base2 s4 not finished (timeout)
- bench_minerva base2 s5 not finished (timeout)
- bench_minerva base s2 not finished (timeout)
- bench_minerva both s1 missing
- bench_minerva both s2 missing
- bench_minerva both s3 missing
- bench_minerva both s4 missing
- bench_minerva both s5 missing
- bench_minerva linear s3 not finished (timeout)
- bench_minerva linear s4 not finished (timeout)
- bench_minerva linear s5 not finished (timeout)
- bench_neorv32 both s1 missing
- bench_neorv32 both s2 missing
- bench_neorv32 both s3 missing
- bench_neorv32 both s4 missing
- bench_neorv32 both s5 missing
- bench_picorv32 both s1 missing
- bench_picorv32 both s2 missing
- bench_picorv32 both s3 missing
- bench_picorv32 both s4 missing
- bench_picorv32 both s5 missing
- bench_vexii model2 s1 not finished (timeout)
- bench_vexii model s1 not finished (timeout)
- bench_vexii both s1 missing
- bench_vexii both s2 missing
- bench_vexii both s3 missing
- bench_vexii both s4 missing
- bench_vexii both s5 missing
- bench_vexriscv base2 s2 not finished (timeout)
- bench_vexriscv base2 s5 not finished (timeout)
- bench_vexriscv both s1 missing
- bench_vexriscv both s2 missing
- bench_vexriscv both s3 missing
- bench_vexriscv both s4 missing
- bench_vexriscv both s5 missing
- bench_vexsmp2 base2 s1 not finished (timeout)
- bench_vexsmp2 base2 s2 not finished (timeout)
- bench_vexsmp2 base2 s3 not finished (timeout)
- bench_vexsmp2 base2 s4 not finished (timeout)
- bench_vexsmp2 base2 s5 not finished (timeout)
- bench_vexsmp2 both s5 missing
- bench_vexsmp2 linear s1 not finished (timeout)
- bench_vexsmp2 linear s2 not finished (timeout)
- multi1 both s1 missing
- multi1 both s2 missing
- multi1 both s3 missing
- multi1 both s4 missing
- multi1 both s5 missing
- multi1 both s6 missing
- multi1 both s7 missing
- multi1 both s8 missing
- multi1 both s9 missing
- multi1 both s10 missing
- multi1 both s11 missing
- multi1 both s12 missing

## Notes

- **Confound:** `runs/` (base, both, linear) was made with `nextpnr-himbaechel.distance.exe` (00:40), which packs a different netlist
  than the two new binaries (the first `Checksum:` after packing differs on every design; the newer code adds the partition /
  clocked-port changes, async output paths, the fused-LUT2 and L20/multiplexer move repairs). So model2/base, model2/both,
  model2/linear, base2/base and base2/linear mix the estimate change with those code changes; e.g. base2 and linear already
  differ at the placement estimate (multi1 s1: 59.81 vs 46.81 MHz). Only **model2/model is clean**: `runs_model/` and this
  directory pack, place and route identically (same checksums and same Fmax up to the first post-route report); they differ only
  in the repairs / LUT swap (model vs tables). A clean base2 reference would be a linear run of the current or the
  distance_model binary.
- Timeouts (3600 s): model2 corescore25 s1/s3 and vexii s1 time out in the router, as model does on the same seeds (shared
  placement). base2: minerva 5/5, vexsmp2 5/5, vexriscv s2/s5; most in the cell-move repairs (hundreds to 1500 rounds of
  "not routable, undone" after a 230-3000 s route), minerva s5 and vexsmp2 s4/s5 never finished routing.
- Cosmetic: with no tables the repair log still says "... ns more slack by the tables".
- Run with `compare.py` unchanged, through `scratchpad/bench_model2/run_model2.py` (sets BINARY to
  `build/Release/nextpnr-himbaechel.distance_model2.exe`, snapshotted as `...distance_model2.exe.runs_model2`, and adds the
  variants model2/base2 with `--chipdb` = the scratchpad copy of `build/himbaechel/uarch/gatemate/chipdb-CCGM1A1.bin`).
  Pairing: `scratchpad/bench_model2/pair2.py`.
