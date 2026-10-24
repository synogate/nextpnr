#!/usr/bin/env python3
"""Placement that learns from routing: place and route, take each net's criticality after routing,
and place again with the placer treating it as a minimum (--vopt place-crit-in). Same seed in every
iteration, so only the learnt criticality changes the placement.

Usage: place_learn.py OUTDIR DESIGN SEED ITERATIONS [--mode max|ema] -- nextpnr options..."""
import os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
outdir, design, seed, iterations = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
rest = sys.argv[5:]
mode, step = "max", 0.1
if rest[:1] == ["--mode"]:
    mode, rest = rest[1], rest[2:]
if rest[:1] == ["--step"]:
    step, rest = float(rest[1]), rest[2:]
if rest[:1] == ["--"]:
    rest = rest[1:]
binary = os.path.join(ROOT, "build", "nextpnr-himbaechel.plearn")
os.makedirs(outdir, exist_ok=True)
args_file = design[:-5] + ".args"
extra = open(args_file).read().split() if os.path.exists(args_file) else []
learnt = {}
learnt_file = os.path.join(outdir, "learnt_s%d.tsv" % seed)
history = []
for it in range(iterations):
    out_file = os.path.join(outdir, "crit_s%d_i%d.tsv" % (seed, it))
    log = os.path.join(outdir, "s%d_i%d.log" % (seed, it))
    cmd = [binary, "--device", "CCGM1A1", "--json", design, "--seed", str(seed), "--vopt", "allow-unconstrained"] + extra + rest
    if learnt:
        cmd += ["--vopt", "place-crit-in=" + learnt_file]
        if mode == "scale":
            cmd += ["--vopt", "place-crit-mode=scale"]
    cmd += ["--vopt", "place-crit-out=" + out_file]
    start = time.time()
    with open(log, "w") as f:
        subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT)
    text = open(log, errors="replace").read()
    fmax = [float(x) for x in re.findall(r"Max frequency for clock '[^']*': ([0-9.]+) MHz", text)]
    history.append(fmax[-1] if "Program finished" in text and fmax else 0.0)
    print("seed %d iteration %d: %.2f MHz (%.0f s)" % (seed, it, history[-1], time.time() - start), flush=True)
    if not os.path.exists(out_file):
        break
    crits = {}
    for line in open(out_file):
        name, crit = line.split("\t")
        crits[name] = float(crit)
    if mode == "scale":
        # an importance per net: up on the longest paths, down where there is slack, in small steps
        for name in set(crits) | set(learnt):
            c = crits.get(name, 0.0)
            learnt[name] = min(4.0, max(0.25, learnt.get(name, 1.0) * (1 + step * (2 * c - 1))))
    for name, crit in crits.items():
        if mode == "max":
            learnt[name] = max(learnt.get(name, 0.0), crit)
        elif mode == "ema":
            learnt[name] = learnt.get(name, 0.0) + 0.5 * (crit - learnt.get(name, 0.0))
    with open(learnt_file, "w") as f:
        for name, crit in learnt.items():
            f.write("%s\t%g\n" % (name, crit))
print("seed %d: %s, best %.2f" % (seed, " ".join("%.2f" % x for x in history), max(history)))
