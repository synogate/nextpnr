# Check the routing of a design saved by a GUI test (e.g. routed_3c.json of routing_3c.txt) without the
# GUI: every net's wires form one tree (one root, every other wire driven by a pip from a wire of the
# net, pip destination = wire, wire bound to that net) and no wire or pip is used twice.
#   build/Release/nextpnr-himbaechel.exe --device CCGM1A1 --json routed_3c.json --run tools/gui_tests/check_routed.py
bad = 0
seen = {}
nets = 0
for name, ni in ctx.nets:
    T = {str(w): m.pip for w, m in ni.wires}
    if not T:
        continue
    nets += 1
    roots = [w for w in T if not T[w] or ctx.getPipSrcWire(T[w]) not in T]
    errs = []
    if len(roots) != 1:
        errs.append("%d roots %s" % (len(roots), roots[:4]))
    for w, p in T.items():
        if p and ctx.getPipDstWire(p) != w:
            errs.append("pip %s does not drive %s" % (p, w))
        b = ctx.getBoundWireNet(w)
        if b is None or b.name != name:
            errs.append("wire %s bound to %s" % (w, None if b is None else b.name))
        if w in seen:
            errs.append("wire %s also in net %s" % (w, seen[w]))
        seen[w] = name
    if errs:
        bad += 1
        print("BAD", name, "; ".join(errs[:5]))
print("CHECK nets with routing: %d, bad: %d" % (nets, bad))
