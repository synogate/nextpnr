# Factored routing heuristic: measurements

Scripts behind `docs/routing_heuristic_concept.md`. They work on a dump of the pip graph of the empty
CCGM1A1 (WORST / SPEED), central window X30..140, Y30..110 (7.0 M pips, 4.3 M nodes). Run them from one
scratch directory; they write their intermediate files (`pips.tsv` is 600 MB) there.

step | script | what
---|---|---
1 | `nextpnr-himbaechel --device CCGM1A1 --run dump.py` | pips of the window with delay (ps) -> `pips.tsv`
2 | `python load.py`, `python labels.py` | CSR graph `graph.pkl`; node -> (tile, local wire name) from the pip names, `labels.pkl` (nodes are named after their lexicographically first wire, so `SB_BIG.Pxx.Y3` shows up as `D3_3_n` of another tile)
- | `dij.py` | Dijkstra helpers used by the rest
3 | `classes.py` | pip delays by class (plane numbers folded), tiles X72..87, Y60..75
4 | `e1.py` | exact least delay from one CPE's outputs along the four directions, 1..36 CPEs
5 | `e2.py` | exact least delay from both outputs of 8 source CPEs (all phases) to the 11 inputs of every CPE within +-30 -> `e2.pkl` (the reference for the prototype)
6 | `e3.py`, `a4.py` | switch-box lattice only (IM, CPE, OM wires excluded) from an SB's D0: straight runs, cost per plane ring step
7 | `e5.py` | CP lines vs fabric for the inputs reachable from CP lines (TI2/4/6/8, CLK, EN)
8 | `proto_build.py` | prototype tables: exits (CPE output -> first SB Y outputs), entries (SB Y outputs -> CPE input, reverse), lattice per SB class on plane 1 -> `proto.pkl`
9 | `proto_lat2.py` | lattice tables with plane changes (all planes) -> `lat2.pkl`
10 | `proto_eval.py 220 600` | prototype, plane changes as 220 ps per ring step
10 | `proto_eval2.py 220 600` | prototype, plane-exact lattice
10 | `proto_eval3.py 220 600 8` | prototype, plane-exact lattice within 8 tiles, ring steps beyond
11 | `a6.py`, `a7.py` | period-20 test of the lattice (one D7 hop per 20 CPEs) near and far
12 | `a8.py` | turn penalty in the lattice
13 | `proto_axis.py 350` | the axis-table variant (RUN per SB class/direction/length, TURN at the corner incl. diagonal step, RING + one turn for a plane change without a turn); needs `proto.pkl` and `e2.pkl`
14 | `a9.py`, `a10.py full|quad` | separable tables indexed by CPE classes and tile offset, built and fitted (the variant that is not good enough)

C++ (`--vopt lookahead=axis`):

script | what
---|---
`dmg_soc_run.sh VARIANT SEED` | dmg_soc with the Makefile flags, repairs off; VARIANT table, axis or hybrid; uses a copy of the binary (`nextpnr-himbaechel.axis2.exe`)
`dmg_soc_summary.py` | TNS, failing endpoints, Fmax of `cc_bufg_inst_O`, router2 time and iterations per seed and variant
