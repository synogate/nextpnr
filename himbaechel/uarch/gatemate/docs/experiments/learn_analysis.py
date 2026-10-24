#!/usr/bin/env python3
"""Does learning beat luck? From route-runs logs: individual runs by index (not the running best),
their slope over the runs, and each mode's final best against plain restarts (route-learn=none),
paired by placement seed."""
import glob, re, statistics, sys
from math import comb

outdir = sys.argv[1] if len(sys.argv) > 1 else "runs12"
runs = {}  # mode -> seed -> [first, run1, run2, ...]
for path in glob.glob(outdir + "/*.log"):
    m = re.match(r".*/multi\d+\.(\w+)\.s(\d+)\.log", path)
    text = open(path, errors="replace").read()
    start = re.search(r"starting at ([0-9.]+) MHz", text)
    if not m or not start:
        continue
    seq = [float(start.group(1))] + [float(x) for x in re.findall(r"run \d+: ([0-9.]+) MHz", text)]
    runs.setdefault(m.group(1), {})[int(m.group(2))] = seq

def slope(ys):
    xs = range(len(ys))
    mx, my = statistics.mean(xs), statistics.mean(ys)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)

def sign_p(w, n):
    k = min(w, n - w)
    return min(1.0, 2 * sum(comb(n, i) for i in range(k + 1)) / 2 ** n) if n else float("nan")

print("mean Fmax of the individual runs, by run (0 = the first routing), over placements:")
for mode, seqs in sorted(runs.items()):
    n = min(len(s) for s in seqs.values())
    means = [statistics.mean(s[i] for s in seqs.values()) for i in range(n)]
    later = statistics.mean(x for s in seqs.values() for x in s[n // 2 + 1:n])
    early = statistics.mean(x for s in seqs.values() for x in s[1:n // 2 + 1])
    print("  %-12s %s  | slope %+.2f MHz/run, runs 1-%d %.1f, runs %d-%d %.1f" % (
        mode, " ".join("%4.1f" % x for x in means), statistics.mean(slope(s[1:n]) for s in seqs.values()),
        n // 2, early, n // 2 + 1, n - 1, later))
if "r10_none" in runs or "none" in runs:
    base = runs.get("r10_none", runs.get("none"))
    print("final best against plain restarts, paired by placement:")
    for mode, seqs in sorted(runs.items()):
        pairs = [(max(seqs[s]), max(base[s])) for s in seqs if s in base]
        ratios = [a / b for a, b in pairs]
        wins = sum(1 for r in ratios if r > 1)
        losses = sum(1 for r in ratios if r < 1)
        print("  %-12s best %.1f vs %.1f MHz, geomean %+.1f%%, %d better / %d worse, sign p %.2f" % (
            mode, statistics.mean(a for a, _ in pairs), statistics.mean(b for _, b in pairs),
            100 * (statistics.geometric_mean(ratios) - 1), wins, losses, sign_p(wins, wins + losses)))
