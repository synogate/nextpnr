#!/usr/bin/env python3
"""Run nextpnr-himbaechel (GateMate) on designs x variants x seeds in parallel and compare.

Usage: compare.py OUTDIR --designs a.json b.json --seeds 1-5 --variants base route place both [-j 12]

Logs go to OUTDIR/<design>.<variant>.s<seed>.log; existing complete logs are reused, so a run can be
extended or resumed. The summary pairs each variant with "base" on the same design and seed.
"""
import argparse, concurrent.futures, os, re, statistics, subprocess, sys, time

# The repository root: the nearest directory above holding build/nextpnr-himbaechel
ROOT = os.path.dirname(os.path.abspath(__file__))
while not os.path.exists(os.path.join(ROOT, "build", "nextpnr-himbaechel")) and os.path.dirname(ROOT) != ROOT:
    ROOT = os.path.dirname(ROOT)
BINARY = os.path.join(ROOT, "build", "nextpnr-himbaechel")
# The router has no iteration limit and sometimes never resolves its last overused wires
TIMEOUT = 3600

VARIANTS = {
    "base": [],
    "route": ["lookahead=table"],
    "place": ["place-delay=table"],
    "both": ["lookahead=table", "place-delay=table"],
    "route_w1": ["lookahead=table", "router-estimate-weight=1.0"],
    "both_w1": ["lookahead=table", "place-delay=table", "router-estimate-weight=1.0"],
    # ENV:NAME=VALUE sets an environment variable (for temporary experiments in the code)
    "both_pr": ["lookahead=table", "place-delay=table", "ARG:--parallel-refine"],
    # extra router cost of the switch box Y outputs / small switch box diagonal muxes (ns)
    "y1": ["lookahead=table", "place-delay=table", "ARG:--parallel-refine", "sb-y-cost=0.1"],
    "y3": ["lookahead=table", "place-delay=table", "ARG:--parallel-refine", "sb-y-cost=0.3"],
    "d3": ["lookahead=table", "place-delay=table", "ARG:--parallel-refine", "sb-ydiag-cost=0.3"],
    "yd3": ["lookahead=table", "place-delay=table", "ARG:--parallel-refine", "sb-y-cost=0.3", "sb-ydiag-cost=0.3"],
    "swap": ["lookahead=table", "place-delay=table", "ARG:--parallel-refine", "lut-input-swap"],
    # deterministic placement (no parallel refine) for paired comparisons
    "loop": ["lookahead=table", "place-delay=table", "lut-swap-rounds=20"],
    "loop10": ["lookahead=table", "place-delay=table", "lut-swap-rounds=10"],
    "tmg": ["lookahead=table", "place-delay=table", "ARG:--tmg-ripup"],
    "pairs10": ["lookahead=table", "place-delay=table", "lut-swap-rounds=10", "lut-swap-moves=pairs"],
    # routing again from scratch, learning criticality (or not: "none")
    "rr_none": ["lookahead=table", "place-delay=table", "route-runs=5", "route-learn=none"],
    "rr_max": ["lookahead=table", "place-delay=table", "route-runs=5", "route-learn=max"],
    "rr_max4": ["lookahead=table", "place-delay=table", "route-runs=5", "route-learn=max4"],
    "rr_scale": ["lookahead=table", "place-delay=table", "route-runs=5", "route-learn=scale"],
    "r10_none": ["lookahead=table", "place-delay=table", "route-runs=10", "route-learn=none"],
    "r10_max": ["lookahead=table", "place-delay=table", "route-runs=10", "route-learn=max"],
    "r10_ema25": ["lookahead=table", "place-delay=table", "route-runs=10", "route-learn=ema", "route-learn-step=0.25"],
    "r10_ema10": ["lookahead=table", "place-delay=table", "route-runs=10", "route-learn=ema", "route-learn-step=0.1"],
    "r10_scale10": ["lookahead=table", "place-delay=table", "route-runs=10", "route-learn=scale", "route-learn-step=0.1"],
    # A* weight for critical connections (estimate weight 1.25 for the others)
    "cw10": ["lookahead=table", "place-delay=table", "router-critical-estimate-weight=1.0"],
    "cw11": ["lookahead=table", "place-delay=table", "router-critical-estimate-weight=1.1"],
    "cw15": ["lookahead=table", "place-delay=table", "router-critical-estimate-weight=1.5"],
    # repairing the critical paths' routing after routing
    "rep20": ["lookahead=table", "place-delay=table", "route-repair=20"],
    "b07_rep20": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "route-repair=20"],
    "all_ce4": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "route-repair=20", "ARG:--placer-heap-critexp=4"],
    "all_cp": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "route-repair=20", "clk-cp"],
    "all_repl": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "route-repair=20", "replicate-fanout=8"],
    "rep20_repl": ["lookahead=table", "place-delay=table", "route-repair=20", "replicate-fanout=8"],
    "rep20_prep30": ["lookahead=table", "place-delay=table", "route-repair=20", "place-repair=30"],
    "nrep20_rep20": ["lookahead=table", "place-delay=table", "net-repair=20", "route-repair=20"],
    "rep50_prep100": ["lookahead=table", "place-delay=table", "route-repair=50", "place-repair=100"],
    "all_rep50_prep100": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "route-repair=50", "place-repair=100"],
    "fuse_rep50_prep100": ["lookahead=table", "place-delay=table", "fuse-luts", "route-repair=50", "place-repair=100"],
    "all_fuse_rep50_prep100": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "fuse-luts", "route-repair=50", "place-repair=100"],
    "fuse2_rep50_prep100": ["lookahead=table", "place-delay=table", "fuse-luts", "route-repair=50", "place-repair=100"],
    "all_fuse2_rep50_prep100": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "fuse-luts", "route-repair=50", "place-repair=100"],
    "en8_rep50_prep100": ["lookahead=table", "place-delay=table", "enable-to-lut=8", "route-repair=50", "place-repair=100"],
    "all_en8_rep50_prep100": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "enable-to-lut=8", "route-repair=50", "place-repair=100"],
    "mux_rep50_prep100": ["lookahead=table", "place-delay=table", "mux-chains", "route-repair=50", "place-repair=100"],
    "all_mux_rep50_prep100": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "mux-chains", "route-repair=50", "place-repair=100"],
    # With the simple timing model fixes (TEMPORARY, until fixed upstream)
    "base_tf": ["timing-fixes"],
    "rep50_prep100_tf": ["timing-fixes", "lookahead=table", "place-delay=table", "route-repair=50", "place-repair=100"],
    "all_rep50_prep100_tf": ["timing-fixes", "lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "route-repair=50", "place-repair=100"],
    "mux_rep50_prep100_tf": ["timing-fixes", "lookahead=table", "place-delay=table", "mux-chains", "route-repair=50", "place-repair=100"],
    "all_mux_rep50_prep100_tf": ["timing-fixes", "lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "mux-chains", "route-repair=50", "place-repair=100"],
    "all_fuse_rep50_prep100_tf": ["timing-fixes", "lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "fuse-luts", "route-repair=50", "place-repair=100"],
    "all_en8_rep50_prep100_tf": ["timing-fixes", "lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "enable-to-lut=8", "route-repair=50", "place-repair=100"],
    "all_muxfuse_rep50_prep100_tf": ["timing-fixes", "lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "mux-chains", "fuse-luts", "route-repair=50", "place-repair=100"],
    "fuse_rep50_prep100_tf": ["timing-fixes", "lookahead=table", "place-delay=table", "fuse-luts", "route-repair=50", "place-repair=100"],
    "en8_rep50_prep100_tf": ["timing-fixes", "lookahead=table", "place-delay=table", "enable-to-lut=8", "route-repair=50", "place-repair=100"],
    "all_muxfuse_cpe1_rep50_prep100_tf": ["timing-fixes", "lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "mux-chains", "fuse-luts", "cpe-merge=1", "route-repair=50", "place-repair=100"],
    "cpe1_rep50_prep100_tf": ["timing-fixes", "lookahead=table", "place-delay=table", "cpe-merge=1", "route-repair=50", "place-repair=100"],
    # The same as rep50_prep100 / all_rep50_prep100 (writing the file does not change the result),
    # writing each net's criticality; then the rewrites only where those were critical
    "rep50_prep100_co": ["lookahead=table", "place-delay=table", "route-repair=50", "place-repair=100", "place-crit-out={crit_out}"],
    "crit_rew_rep50_prep100": ["lookahead=table", "place-delay=table", "route-repair=50", "place-repair=100",
                               "mux-chains", "fuse-luts", "enable-to-lut=8", "rewrite-crit-in={crit_of:rep50_prep100_co}"],
    "all_rep50_prep100_co": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "route-repair=50", "place-repair=100", "place-crit-out={crit_out}"],
    "all_crit_rew_rep50_prep100": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "route-repair=50", "place-repair=100",
                                   "mux-chains", "fuse-luts", "enable-to-lut=8", "rewrite-crit-in={crit_of:all_rep50_prep100_co}"],
    "all_prep30": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "route-repair=20", "place-repair=30"],
    # timing-driven placement
    "topt": ["lookahead=table", "place-delay=table", "timing-opt"],
    "tw30": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-timingweight=30"],
    "ce4": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-critexp=4"],
    "b07_topt": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7", "timing-opt"],
    # placement density limit of the analytic placer (default 0.9)
    "b07": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.7"],
    "b05": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.5"],
    "b03": ["lookahead=table", "place-delay=table", "ARG:--placer-heap-beta=0.3"],
    "moves10": ["lookahead=table", "place-delay=table", "lut-swap-rounds=10", "lut-swap-moves=both"],
}


def sign_test(wins, n):
    """Two-sided p-value that wins out of n paired comparisons are this lopsided by chance."""
    from math import comb
    k = min(wins, n - wins)
    return min(1.0, 2 * sum(comb(n, i) for i in range(k + 1)) / 2 ** n) if n else float("nan")


def seeds(text):
    out = []
    for part in text.split(","):
        a, _, b = part.partition("-")
        out += list(range(int(a), int(b or a) + 1))
    return out


def log_path(outdir, design, variant, seed):
    return os.path.join(outdir, "%s.%s.s%d.log" % (os.path.basename(design)[:-5], variant, seed))


def run(outdir, design, variant, seed):
    path = log_path(outdir, design, variant, seed)
    if os.path.exists(path):  # finished, or failed or killed (no "Program finished")
        return path
    cmd = [BINARY, "--device", "CCGM1A1", "--json", design, "--seed", str(seed), "--vopt", "allow-unconstrained"]
    # Per-design arguments (e.g. constraint files) from <design>.args next to it
    args_file = design[:-5] + ".args"
    if os.path.exists(args_file):
        cmd += open(args_file).read().split()
    env = dict(os.environ)
    # {crit_out}: this run's criticality file; {crit_of:VARIANT}: the one VARIANT wrote for the same
    # design and seed (run that variant first)
    def crit_file(of):
        return os.path.join(outdir, "%s.%s.s%d.crit.tsv" % (os.path.basename(design)[:-5], of, seed))
    def expand(opt):
        opt = opt.replace("{crit_out}", crit_file(variant))
        for of in re.findall(r"\{crit_of:(\w+)\}", opt):
            if not os.path.exists(crit_file(of)):
                raise RuntimeError("%s needs %s's criticality file; run it first" % (variant, of))
            opt = opt.replace("{crit_of:%s}" % of, crit_file(of))
        return opt
    for opt in map(expand, VARIANTS[variant]):
        if opt.startswith("ENV:"):
            key, _, value = opt[4:].partition("=")
            env[key] = value
        elif opt.startswith("ARG:"):
            cmd.append(opt[4:])
        else:
            cmd += ["--vopt", opt]
    start = time.time()
    with open(path + ".tmp", "w") as log:
        log.write("# " + " ".join(cmd) + "\n")
        log.flush()
        try:
            subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=TIMEOUT, env=env)
        except subprocess.TimeoutExpired:
            log.write("# timeout\n")
        log.write("# wall %.1f\n" % (time.time() - start))
    os.replace(path + ".tmp", path)
    return path


def parse(path):
    text = open(path, errors="replace").read()
    fmax = [float(x) for x in re.findall(r"Max frequency for clock '[^']*': ([0-9.]+) MHz", text)]
    num = lambda pattern: (lambda m: float(m.group(1)) if m else None)(re.search(pattern, text))
    iters = re.findall(r"iter=(\d+) wires=", text)
    return {
        "ok": "Program finished" in text and len(fmax) >= 2,
        "place_fmax": fmax[0] if fmax else None,
        "fmax": fmax[-1] if len(fmax) >= 2 else None,
        "route_s": num(r"Router2 time ([0-9.]+)s"),
        "heap_s": num(r"HeAP Placer Time: ([0-9.]+)s"),
        "sa_s": num(r"SA placement time ([0-9.]+)s"),
        "table_s": num(r"Built the GateMate delay tables:.* ([0-9.]+)s"),
        "wall_s": num(r"# wall ([0-9.]+)"),
        "iters": int(iters[-1]) if iters else None,
        "wires": int(re.findall(r"iter=\d+ wires=(\d+)", text)[-1]) if iters else None,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--designs", nargs="+", required=True)
    ap.add_argument("--seeds", default="1-5")
    ap.add_argument("--variants", nargs="+", default=["base", "route", "place", "both"])
    ap.add_argument("-j", type=int, default=12)
    ap.add_argument("--summary-only", action="store_true")
    ap.add_argument("--reference", default="base", help="variant the others are paired with")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)
    jobs = [(d, v, s) for d in args.designs for s in seeds(args.seeds) for v in args.variants]
    # Run a copy of the binary (next to it, for the chipdb), so that rebuilding cannot change a
    # comparison half way; delete the copy to pick up a new build
    global BINARY
    snapshot = BINARY + "." + os.path.basename(os.path.normpath(args.outdir))
    if not args.summary_only and not os.path.exists(snapshot):
        import shutil
        shutil.copy2(BINARY, snapshot)
    BINARY = snapshot
    if not args.summary_only:
        # biggest designs first, so the tail of the run is short
        jobs.sort(key=lambda j: -os.path.getsize(j[0]))
        with concurrent.futures.ThreadPoolExecutor(args.j) as pool:
            futures = [pool.submit(run, args.outdir, *j) for j in jobs]
            for n, f in enumerate(concurrent.futures.as_completed(futures), 1):
                f.result()
                print("\r%d/%d runs" % (n, len(jobs)), end="", file=sys.stderr, flush=True)
        print(file=sys.stderr)

    results = {}
    for d, v, s in jobs:
        path = log_path(args.outdir, d, v, s)
        if os.path.exists(path):
            results[(d, v, s)] = parse(path)
    med = lambda xs: statistics.median([x for x in xs if x is not None]) if [x for x in xs if x is not None] else float("nan")
    rows = ["design | variant | routed | Fmax median | Fmax min-max | vs reference: geomean | median | wins | sign test p | place est. | route s | SA s | wall s | wires",
            "---|---|---|---|---|---|---|---|---|---|---|---|---|---"]
    csv = ["design,variant,seed,ok,place_fmax,fmax,route_s,heap_s,sa_s,table_s,wall_s,iters,wires"]
    for d in args.designs:
        name = os.path.basename(d)[:-5]
        for v in args.variants:
            rs = [(s, results.get((d, v, s))) for s in seeds(args.seeds)]
            rs = [(s, r) for s, r in rs if r]
            for s, r in rs:
                csv.append(",".join(str(x) for x in [name, v, s] + [r[k] for k in
                           ["ok", "place_fmax", "fmax", "route_s", "heap_s", "sa_s", "table_s", "wall_s", "iters", "wires"]]))
            good = [(s, r) for s, r in rs if r["ok"]]
            f = [r["fmax"] for _, r in good]
            diffs = []
            for s, r in good:
                b = results.get((d, args.reference, s))
                if b and b["ok"]:
                    diffs.append(100.0 * (r["fmax"] / b["fmax"] - 1))
            wins = sum(1 for x in diffs if x > 0)
            ties = sum(1 for x in diffs if x == 0)
            geo = 100 * (statistics.geometric_mean([1 + x / 100 for x in diffs]) - 1) if diffs else 0
            show = diffs and v != args.reference
            rows.append("%s | %s | %d/%d | %.2f | %.2f-%.2f | %s | %s | %s | %s | %.2f | %.0f | %.0f | %.0f | %.0f" % (
                name, v, len(good), len(rs), med(f), min(f) if f else 0, max(f) if f else 0,
                "%+.1f%%" % geo if show else "",
                "%+.1f%%" % med(diffs) if show else "",
                "%d/%d" % (wins, len(diffs)) if show else "",
                "%.3f" % sign_test(wins, len(diffs) - ties) if show else "",
                med([r["place_fmax"] for _, r in good]), med([r["route_s"] for _, r in good]),
                med([r["sa_s"] for _, r in good]), med([r["wall_s"] for _, r in good]),
                med([r["wires"] for _, r in good])))
    open(os.path.join(args.outdir, "results.csv"), "w").write("\n".join(csv) + "\n")
    open(os.path.join(args.outdir, "summary.md"), "w").write("\n".join(rows) + "\n")
    print("\n".join(rows))


if __name__ == "__main__":
    main()
