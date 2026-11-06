# The state of a design in detail, to check that saving and loading it loses nothing (tools/gui_tests/saveload_*.txt).
# In a GUI test (the interpreter starts afresh for each design loaded, so after every openjson):
#   py import os; exec(open(os.path.join(os.path.dirname(os.environ["NEXTPNR_GUI_TEST"]), "design_state.py")).read())
#   py save_state("stage1_state.json")         (right after savejson: the state written)
#   py D = compare_states(load_state("stage1_state.json"), dump_state()); print(len(D), D[:20])
import json

# Attributes the save adds for what the packer worked out, and which loading takes back into the design (see
# GateMateImpl::archStateToAttributes); the comparison looks at what they stand for (clusters, regions) instead
HELPER_ATTRS = ("GATEMATE_CLUSTER", "GATEMATE_CONSTR", "GATEMATE_MULT", "GATEMATE_CONSTR_X", "GATEMATE_REGION",
                "GATEMATE_PORT_CELL", "GATEMATE_MULT_CLUSTER", "GATEMATE_DDR", "GATEMATE_PARTITION_CLOCK",
                "GATEMATE_GLOBAL", "NEXTPNR_CLOCK", "NEXTPNR_BEL", "BEL_STRENGTH", "ROUTING")


def dump_state():
    S = {"settings": dict(ctx.getSettings())}
    cells = {}
    for n, c in ctx.cells:
        children = sorted([list(x) for x in ctx.getClusterChildren(n)])
        cells[n] = {
            "type": c.type, "bel": str(c.bel) if c.bel else "", "strength": int(c.belStrength) if c.bel else -1,
            "cluster": ctx.getCellCluster(n), "children": children, "region": ctx.getCellRegion(n),
            "ports": {pn: [int(p.type), p.net.name if p.net else None] for pn, p in c.ports},
            "params": {k: str(v) for k, v in c.params},
            "attrs": {k: str(v) for k, v in c.attrs if k not in HELPER_ATTRS}}
    S["cells"] = cells
    nets = {}
    for n, ni in ctx.nets:
        # (a net connecting nothing, without routing, is not written: the JSON has nothing that refers to it)
        if not ni.driver.cell and not list(ni.users) and not list(ni.wires):
            continue
        nets[n] = {
            "driver": [ni.driver.cell.name, ni.driver.port] if ni.driver.cell else None,
            "users": sorted([[u.cell.name, u.port] for u in ni.users]),
            "wires": {str(w): [str(m.pip) if m.pip else "", int(m.strength)] for w, m in ni.wires},
            "attrs": {k: str(v) for k, v in ni.attrs if k not in HELPER_ATTRS}}
    S["nets"] = nets
    return S


def compare_states(a, b, ignore_settings=("synth",)):
    """The differences between two states (lists of text lines; empty if equal)"""
    diffs = []
    for k in sorted(set(a["settings"]) | set(b["settings"])):
        if k not in ignore_settings and a["settings"].get(k) != b["settings"].get(k):
            diffs.append("setting %s: %r -> %r" % (k, a["settings"].get(k), b["settings"].get(k)))
    ca, cb = a["cells"], b["cells"]
    diffs += ["cell lost: %s (%s)" % (n, ca[n]["type"]) for n in sorted(set(ca) - set(cb))]
    diffs += ["cell new: %s (%s)" % (n, cb[n]["type"]) for n in sorted(set(cb) - set(ca))]
    for n in sorted(set(ca) & set(cb)):
        x, y = ca[n], cb[n]
        for f in ("type", "bel", "strength", "cluster", "children", "region"):
            if x[f] != y[f]:
                diffs.append("cell %s %s: %r -> %r" % (n, f, x[f], y[f]))
        for f in ("ports", "params", "attrs"):
            for k in sorted(set(x[f]) | set(y[f])):
                if x[f].get(k) != y[f].get(k):
                    diffs.append("cell %s %s %s: %r -> %r" % (n, f, k, x[f].get(k), y[f].get(k)))
    na, nb = a["nets"], b["nets"]
    diffs += ["net lost: %s" % n for n in sorted(set(na) - set(nb))]
    diffs += ["net new: %s" % n for n in sorted(set(nb) - set(na))]
    for n in sorted(set(na) & set(nb)):
        x, y = na[n], nb[n]
        for f in ("driver", "users"):
            if x[f] != y[f]:
                diffs.append("net %s %s: %r -> %r" % (n, f, x[f], y[f]))
        if x["wires"] != y["wires"]:
            diff = sorted(set(map(str, x["wires"].items())) ^ set(map(str, y["wires"].items())))
            diffs.append("net %s wires: %d -> %d, differing %r" % (n, len(x["wires"]), len(y["wires"]), diff[:6]))
        for k in sorted(set(x["attrs"]) | set(y["attrs"])):
            if x["attrs"].get(k) != y["attrs"].get(k):
                diffs.append("net %s attr %s: %r -> %r" % (n, k, x["attrs"].get(k), y["attrs"].get(k)))
    return diffs


def save_state(path):
    with open(path, "w") as f:
        json.dump(dump_state(), f, indent=0, sort_keys=True)


def load_state(path):
    with open(path) as f:
        return json.load(f)


def check_state(path):
    """Compare the design with the state saved in path, print the differences; True if there are none"""
    diffs = compare_states(load_state(path), dump_state())
    print("%d differences to %s" % (len(diffs), path))
    for d in diffs[:30]:
        print("  " + d)
    return not diffs


def moved_cells(state, min_strength=4):
    """Cells placed in state with at least min_strength (default: fixed, locked, user; Place may move the others)
    that are on another tile now (postPlace's repack may take unused cells out and rebind the halves of a CPE, so
    tiles are compared, and cells gone are left out)"""
    moved = []
    for n, c in state["cells"].items():
        if c["bel"] and c["strength"] >= min_strength and n in ctx.cells:
            bel = str(ctx.cells[n].bel) if ctx.cells[n].bel else ""
            if bel.split("/")[0] != c["bel"].split("/")[0]:
                moved.append((n, c["bel"], bel))
    return moved


def changed_wires(state, net):
    """The wires of net in state that it has no longer, or with another pip or strength (the router may add wires,
    e.g. continuing hand routing into a CPE)"""
    now = {str(w): [str(m.pip) if m.pip else "", int(m.strength)] for w, m in ctx.nets[net].wires}
    return [(w, v, now.get(w)) for w, v in state["nets"][net]["wires"].items() if now.get(w) != v]
