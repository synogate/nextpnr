#!/usr/bin/env python3
"""Where the critical path's time goes, by logic level, and which levels could merge.

Usage: depth.py LOG NETLIST.json [REPORT_INDEX]

Takes the critical path report before GateMate's post-route netlist step (the second to last one by
default), and the netlist nextpnr wrote with --write. For each logic cell on the path: its type, the
distinct signals it uses, the fanout of its output, the routing into it against the least delay on
the empty chip (offset_map.tsv). A level could merge into the next when the next uses its output and
the two use at most 4 distinct signals together besides it (one CPE half), or at most 8 (a whole CPE,
with the half's L2T5 structure: not every 8-input function fits); "alone" when it feeds nothing else,
else only by duplicating it.
"""
import json, re, sys

log_path, netlist_path = sys.argv[1], sys.argv[2]
index = int(sys.argv[3]) if len(sys.argv) > 3 else -2

least = {}
for line in open("offset_map.tsv"):
    dx, dy, d = map(int, line.split())
    least[(dx, dy)] = d / 1000

text = open(log_path, errors="replace").read()
reports = text.split("Critical path report for clock")
report = reports[index if len(reports) > -index else -1].split("Critical path report for cross-domain")[0]
segments = re.findall(r"^Info: +(clk-to-q|routing|logic|setup) +([0-9.]+) +([0-9.]+) (Source|Net) (\S+)(.*)$", report, re.M)

netlist = json.load(open(netlist_path))
module = next(m for m in netlist["modules"].values())
cells = module["cells"]
CONSTANT = {"0", "1", "x", "z"}


def inputs_of(cell):
    used = set()
    for port, bits in cell["connections"].items():
        if cell["port_directions"].get(port) != "input" or port.startswith(("CLK", "EN", "SR")):
            continue
        used.update(b for b in bits if str(b) not in CONSTANT)
    return used


def outputs_of(cell):
    out = set()
    for port, bits in cell["connections"].items():
        if cell["port_directions"].get(port) == "output":
            out.update(b for b in bits if str(b) not in CONSTANT)
    return out


fanout = {}
for cell in cells.values():
    for bit in inputs_of(cell):
        fanout[bit] = fanout.get(bit, 0) + 1


def short(name):
    return re.sub(r"\$abc\$\d+\$|\$auto\$|blifparse\.cc:\d+:parse_blif|core\[\d+\]\.soc\.", "", name)[:44]


# The logic cells in path order, with the routing into each
path = []  # (cell name, routing delay, least, logic delay)
routing = None
for kind, delay, _, what, name, rest in segments:
    if kind == "routing":
        m = re.search(r"\((\d+),(\d+)\) -> \((\d+),(\d+)\)", rest)
        ldelay = 0
        if m:
            x0, y0, x1, y1 = map(int, m.groups())
            ldelay = 0 if (x0, y0) == (x1, y1) else least.get((x1 - x0, y1 - y0), 0)
        routing = (float(delay), ldelay)
    elif kind == "logic":
        cell = name.rsplit(".", 1)[0]
        path.append((cell, routing, float(delay)))
        routing = None

total_logic = sum(float(d) for k, d, *_ in segments if k in ("logic", "clk-to-q", "setup"))
total_routing = sum(float(d) for k, d, *_ in segments if k == "routing")
print("%s: %d logic levels, logic %.2f ns, routing %.2f ns, end %.2f ns" % (
    log_path, len(path), total_logic, total_routing, float(segments[-1][2]) if segments else 0))
print("%-44s %-10s %3s %3s %11s %5s  %s" % ("cell", "type", "in", "fan", "route/least", "logic", "merge with the next"))
merges = {"4 alone": 0, "4 dup": 0, "8 alone": 0, "8 dup": 0}
for i, (name, route, logic) in enumerate(path):
    cell = cells.get(name)
    if cell is None:
        print("%-44s (not in the netlist)" % short(name))
        continue
    ins, outs = inputs_of(cell), outputs_of(cell)
    fan = sum(fanout.get(b, 0) for b in outs)
    note = ""
    if i + 1 < len(path) and path[i + 1][0] in cells:
        nxt = cells[path[i + 1][0]]
        if outs & inputs_of(nxt):
            together = (ins | inputs_of(nxt)) - outs
            alone = fan == 1
            for limit in (4, 8):
                if len(together) <= limit:
                    key = "%d %s" % (limit, "alone" if alone else "dup")
                    merges[key] += 1
                    note = "%d signals together: fits %s%s" % (len(together), "a half" if limit == 4 else "a CPE",
                                                              "" if alone else ", duplicating it (fanout %d)" % fan)
                    break
            else:
                note = "%d signals together" % len(together)
    r = "%5.2f/%5.2f" % route if route else ""
    print("%-44s %-10s %3d %3d %11s %5.2f  %s" % (short(name), cell["type"][:10], len(ins), fan, r, logic, note))
print("levels that could merge: %s" % ", ".join("%s: %d" % kv for kv in merges.items()))
