# Distance model / delay estimate runs

Benchmark runs for `docs/distance_model.md`, made with `docs/experiments/compare.py` (logs
`<design>.<variant>.s<seed>.log`, `results.csv`, `summary.md` per directory). Each directory was run with a
snapshot of the binary, `build/Release/nextpnr-himbaechel.<name>.exe` (compare.py also keeps a copy per
output directory, `<binary>.<dir>`). Only compare results within one branch: `runs/` is from the planemate
branch, all other directories from main-branch snapshots (the uncommitted distance-model work on main).

directory | branch | binary | contents
---|---|---|---
`runs` | planemate | `nextpnr-himbaechel.distance.exe` (`.runs.exe`) | first baseline: linear / base (old default, tables) / both; not comparable with the rest
`runs_model` | main | `distance_model.exe` | first model for router/placer, tables for the repairs
`runs_model2` | main | `distance_model2.exe` | model2 (model everywhere, no tables), base2 (no option: linear, model repairs)
`runs_model3` | main | `distance_model3.exe` | model3, refined model
`runs_base3` | main | `distance_model3.exe` | reference: table3 (`lookahead=table place-delay=table`), linear3
`runs_model4` | main | `distance_model4.exe` | model4, final model
`runs_split` | main | `distance_model4.exe` | placer vs router split: table4, pm_rt, pt_rm
`runs_hybrid` | main | `distance_hybrid.exe` | hybrid, first round (hyb2/3/4/6, multi1 and Stage B hyb6)
`runs_hybrid2` | main | `distance_hybrid2.exe` | hybrid without the slope search: hyb2/4/4m/6/8/12, `sameload/` (dmg_soc seed 1 one at a time)
`runs_default` | main | `distance_final.exe` | smoke runs, dmg_soc seed 1: default (no option = hybrid radius 4), route_table (`lookahead=table` alone: placer stays hybrid, on the full tables)

The references used in the summaries are table3/linear3 (`runs_base3`) and model4 (`runs_model4`).
