import re, glob, statistics as st, sys
# dmg_soc_summary.py: per variant and seed TNS / failing endpoints / Fmax (cc_bufg_inst_O) / router2 s / placer s
rows = {}
for f in sorted(glob.glob("dmg_soc.*.log")):
    _, v, s, _ = f.split('.')
    t = open(f).read()
    tns = re.findall(r"Total negative slack: (-?[\d.]+) ns over (\d+)", t)
    fm = re.findall(r"Max frequency for clock 'cc_bufg_inst_O': ([\d.]+) MHz", t)
    rt = re.findall(r"Router2 time ([\d.]+)s", t)
    heap = re.findall(r"HeAP Placer Time: ([\d.]+)s", t)
    sa = re.findall(r"SA placement time ([\d.]+)s", t)
    if len(tns) < 2 or not fm or not rt:
        continue
    rows[(v, s)] = dict(tns=float(tns[-1][0]), fail=int(tns[-1][1]), mhz=float(fm[-1]), router=float(rt[-1]),
                        heap=float(heap[-1]) if heap else float('nan'), sa=float(sa[-1]) if sa else float('nan'), place_tns=float(tns[0][0]))
variants = sys.argv[1:] or [v for v in ("table", "axis", "both", "both3") if any(k[0] == v for k in rows)]
seeds = sorted(set(s for _, s in rows), key=lambda x: int(x[1:]))
print("seed " + "".join(f"| {v:>6s}: TNS / fail / MHz / router s / HeAP s / SA s " for v in variants))
for s in seeds:
    line = f"{s:5s}"
    for v in variants:
        r = rows.get((v, s))
        line += "| " + (f"{r['tns']:8.0f} {r['fail']:5d} {r['mhz']:6.2f} {r['router']:6.0f} {r['heap']:6.0f} {r['sa']:6.0f} " if r else " " * 43)
    print(line)
for v in variants:
    rs = [rows[(v, s)] for s in seeds if (v, s) in rows]
    print(f"{v:6s} n={len(rs)} median TNS {st.median(r['tns'] for r in rs):8.0f}  mean TNS {st.mean(r['tns'] for r in rs):8.0f}"
          f"  median fail {st.median(r['fail'] for r in rs):6.0f}  median MHz {st.median(r['mhz'] for r in rs):6.2f}"
          f"  geomean MHz {st.geometric_mean([r['mhz'] for r in rs]):6.2f}  median router {st.median(r['router'] for r in rs):5.0f}s"
          f"  median HeAP {st.median(r['heap'] for r in rs):5.0f}s  median SA {st.median(r['sa'] for r in rs):5.0f}s")
