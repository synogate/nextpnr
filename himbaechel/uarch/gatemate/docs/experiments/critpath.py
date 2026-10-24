#!/usr/bin/env python3
"""Split the final clock-to-clock critical path of each run into logic, the least routing delay on the
empty chip (offset_map.tsv) and the excess above it (detours, congestion, fanout)."""
import glob, re, statistics, sys
best = {}
for line in open("offset_map.tsv"):
    dx, dy, d = map(int, line.split())
    best[(dx, dy)] = d / 1000.0
def least(dx, dy):
    if (dx, dy) in best: return best[(dx, dy)]
    # beyond the map: nearest entry
    k = min(best, key=lambda o: abs(o[0] - dx) + abs(o[1] - dy)); return best[k]
by = {}
for path in sorted(glob.glob(sys.argv[1])):
    text = open(path, errors="replace").read()
    if "Program finished" not in text: continue
    parts = text.split("Critical path report for clock")
    if len(parts) < 2: continue
    report = parts[-1].split("Critical path report for cross-domain")[0]
    logic = sum(float(x) for x in re.findall(r"^Info: +(?:logic|setup|clk-to-q) +([0-9.]+)", report, re.M))
    routing = minimum = 0.0; n = 0
    for d, x0, y0, x1, y1 in re.findall(r"^Info: +routing +([0-9.]+) +[0-9.]+ Net .*? \((\d+),(\d+)\) -> \((\d+),(\d+)\)", report, re.M):
        d = float(d); routing += d; n += 1
        same = (x0, y0) == (x1, y1)
        minimum += min(d, 0 if same else least(int(x1) - int(x0), int(y1) - int(y0)))
    m = re.match(r".*/(multi\d+)\.(\w+)\.s\d+\.log", path)
    by.setdefault((m.group(1), m.group(2)), []).append((logic, routing, minimum, n))
print("%-8s %-6s %5s %7s %8s %9s %8s %6s" % ("design", "var", "runs", "logic", "routing", "least", "excess", "nets"))
for (d, v), rs in sorted(by.items()):
    mean = lambda i: statistics.mean(r[i] for r in rs)
    print("%-8s %-6s %5d %6.1fns %7.1fns %8.1fns %7.1fns %6.1f" % (d, v, len(rs), mean(0), mean(1), mean(2), mean(1) - mean(2), mean(3)))
