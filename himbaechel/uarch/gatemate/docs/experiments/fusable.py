#!/usr/bin/env python3
"""How many LUT cells of a synthesized GateMate netlist could fuse into the LUT they feed.

Usage: fusable.py NETLIST.json

Fusing A into B (B uses A's output): the function of B with A substituted, over their signals
together, must fit one CPE half: L10(L00(p, q), L01(r, s)) for some assignment of at most 4 signals to
p, q, r, s. Counts the A that feed only LUT cells, all of whom could absorb them (then A goes away),
by type; and of those the inverters.
"""
import itertools, json, sys
from collections import Counter

m = [x for x in json.load(open(sys.argv[1]))["modules"].values() if x.get("attributes", {}).get("top")][0]
cells = m["cells"]
CONST = {"0": 0, "1": 1}


def bits(c, p):
    # an unconnected input counts as constant 0 here
    return c["connections"].get(p) or ["0"]


def init(c, name, width=4):
    v = c["parameters"].get(name, "0" * width)
    return int(v, 2) if isinstance(v, str) else int(v)


def lut2(table, a, b):
    return (table >> (a | (b << 1))) & 1


# The function of each LUT-like cell: (inputs as signal bits, evaluate(values by input))
def model(c):
    t = c["type"]
    if t == "CC_LUT1":
        return [bits(c, "I0")[0]], lambda v: (init(c, "INIT", 2) >> v[0]) & 1
    if t == "CC_LUT2":
        return [bits(c, "I0")[0], bits(c, "I1")[0]], lambda v: lut2(init(c, "INIT"), v[0], v[1])
    if t == "CC_L2T4":
        ins = [bits(c, p)[0] for p in ("I0", "I1", "I2", "I3")]
        return ins, lambda v: lut2(init(c, "INIT_L10"), lut2(init(c, "INIT_L00"), v[0], v[1]),
                                   lut2(init(c, "INIT_L01"), v[2], v[3]))
    if t == "CC_MX2":
        return [bits(c, p)[0] for p in ("D0", "D1", "S0")], lambda v: v[1] if v[2] else v[0]
    return None


models = {n: model(c) for n, c in cells.items()}
users = {}
for n, c in cells.items():
    for p, bs in c["connections"].items():
        if c["port_directions"].get(p) == "input":
            for b in bs:
                users.setdefault(b, []).append(n)


def fits_half(signals, f):
    """f: values (tuple by signals) -> 0/1. A half takes 4 positions; unused ones stay out."""
    n = len(signals)
    if n > 4:
        return False
    for perm in itertools.permutations(range(4), n):
        pos = {s: perm[i] for i, s in enumerate(range(n))}  # signal index -> position
        def value(row, col):  # row: (p, q) bits, col: (r, s) bits
            pv = [row & 1, row >> 1, col & 1, col >> 1]
            return f(tuple(pv[pos[i]] for i in range(n)))
        rows = {tuple(value(r, c) for c in range(4)) for r in range(4)}
        cols = {tuple(value(r, c) for r in range(4)) for c in range(4)}
        if len(rows) <= 2 and len(cols) <= 2:
            return True
    return False


absorbable = Counter()
inverters = 0
for a, ma in models.items():
    if ma is None:
        continue
    out = bits(cells[a], "O")[0] if "O" in cells[a]["connections"] else bits(cells[a], "Y")[0]
    sinks = users.get(out, [])
    if not sinks or any(models.get(b) is None for b in sinks):
        continue
    ok = True
    for b in set(sinks):
        ins_b, fb = models[b]
        ins_a, fa = ma
        signals = []
        for s in [s for s in ins_b if s != out] + ins_a:
            if str(s) not in CONST and s not in signals:
                signals.append(s)
        def f(values, ins_a=ins_a, ins_b=ins_b, fa=fa, fb=fb, signals=signals):
            val = dict(zip(signals, values))
            get = lambda s: CONST[str(s)] if str(s) in CONST else val[s]
            av = fa([get(s) for s in ins_a])
            return fb([av if s == out else get(s) for s in ins_b])
        if not fits_half(signals, f):
            ok = False
            break
    if ok:
        absorbable[cells[a]["type"]] += 1
        inverters += cells[a]["type"] == "CC_LUT1"
total = sum(1 for m in models.values() if m is not None)
print("%s: %d LUT cells (LUT1/LUT2/L2T4/MX2), %d could fuse into all their users and go away (%s), %d of them inverters" % (
    sys.argv[1], total, sum(absorbable.values()), dict(absorbable), inverters))
