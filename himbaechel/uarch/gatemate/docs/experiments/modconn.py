# how the top-level modules of a netlist are connected:  modconn.py <json>
import collections, json, sys
d = json.load(open(sys.argv[1]))
top = [m for m in d['modules'].values() if m.get('attributes', {}).get('top')][0]
bitname = {}
for n, v in top['netnames'].items():
    if v.get('hide_name') or n.startswith('$'):
        continue
    for b in v['bits']:
        if isinstance(b, int) and (b not in bitname or n.count('.') > bitname[b].count('.')):
            bitname[b] = n
def module_of(cell):
    names = []
    for port, bits in cell['connections'].items():
        if cell.get('port_directions', {}).get(port) == 'output':
            names += [bitname[b] for b in bits if b in bitname]
    if not names:
        for port, bits in cell['connections'].items():
            names += [bitname[b] for b in bits if b in bitname]
    names = [n for n in names if '.' in n]
    if not names:
        return '?'
    return collections.Counter(n.split('.')[0] for n in names).most_common(1)[0][0]
mod = {}
drv = {}
users = collections.defaultdict(list)
for n, c in top['cells'].items():
    if c['type'] == '$scopeinfo':
        continue
    mod[n] = module_of(c)
    for port, bits in c['connections'].items():
        for b in bits:
            if not isinstance(b, int):
                continue
            if c.get('port_directions', {}).get(port) == 'output':
                drv[b] = n
            else:
                users[b].append(n)
size = collections.Counter(mod.values())
inner = collections.Counter(); outer = collections.Counter(); pair = collections.Counter()
for b, dn in drv.items():
    us = users.get(b, [])
    if len(us) > 64:      # clocks, resets, enables with a huge fanout say nothing about the neighbourhood
        continue
    ms = {mod[u] for u in us}
    if ms <= {mod[dn]}:
        inner[mod[dn]] += 1
    else:
        outer[mod[dn]] += 1
        for m in ms - {mod[dn]}:
            outer[m] += 0
            pair[tuple(sorted((mod[dn], m)))] += 1
print(f"{'module':36s} cells  nets inside  nets leaving  share leaving")
for m, n in size.most_common(24):
    print(f"{m[:36]:36s} {n:5d}  {inner[m]:6d}  {outer[m]:6d}  {100 * outer[m] / max(1, inner[m] + outer[m]):5.1f}%")
print("nets between two modules (driver in one, a user in the other):")
for (a, b), n in pair.most_common(30):
    print(f"  {n:5d}  {a} <-> {b}")
