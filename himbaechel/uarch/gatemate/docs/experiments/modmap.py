# where the modules of a placed netlist are:  modmap.py <placed.json> [out.png] [levels]
import collections, json, math, re, sys
import numpy as np

f = sys.argv[1]
png = sys.argv[2] if len(sys.argv) > 2 else None
levels = int(sys.argv[3]) if len(sys.argv) > 3 else 1
d = json.load(open(f))
top = [m for m in d['modules'].values() if m.get('attributes', {}).get('top')] or list(d['modules'].values())[-1:]
top = top[0]
# a name per net bit: the public name with the most levels of hierarchy
bitname = {}
for n, v in top['netnames'].items():
    if v.get('hide_name') or n.startswith('$'):
        continue
    for b in v['bits']:
        if isinstance(b, int) and (b not in bitname or n.count('.') > bitname[b].count('.')):
            bitname[b] = n


def module_of(name, cell):
    names = []
    if '.' in name and not name.startswith('$'):
        names.append(name)
    for port, bits in cell['connections'].items():
        if cell.get('port_directions', {}).get(port) == 'output':
            names += [bitname[b] for b in bits if b in bitname]
    if not names:
        for port, bits in cell['connections'].items():
            names += [bitname[b] for b in bits if b in bitname]
    names = [n for n in names if '.' in n]
    if not names:
        return None
    c = collections.Counter('.'.join(n.split('.')[:levels]) for n in names)
    return c.most_common(1)[0][0]


cells = []
for n, c in top['cells'].items():
    bel = c.get('attributes', {}).get('NEXTPNR_BEL')
    m = bel and re.match(r'X(\d+)Y(\d+)/(.*)', bel)
    if not m:
        continue
    cells.append((int(m.group(1)), int(m.group(2)), c['type'], module_of(n, c), n))
logic = [c for c in cells if c[2].startswith('CPE')]
mods = collections.Counter(c[3] for c in logic)
print(f"{len(cells)} placed cells, {len(logic)} CPE cells, without a module: {mods.get(None, 0)}")
xy = np.array([(c[0], c[1]) for c in logic], float)
lab = np.array([c[3] or '?' for c in logic])
cx, cy = xy.mean(0)
print(f"design: centre ({cx:.0f}, {cy:.0f}), RMS radius {math.sqrt(((xy - xy.mean(0)) ** 2).sum(1).mean()):.1f}, "
      f"box x {xy[:, 0].min():.0f}..{xy[:, 0].max():.0f} y {xy[:, 1].min():.0f}..{xy[:, 1].max():.0f}")
# neighbours: the share of a cell's 8 nearest cells that are of its module, against the module's share of all cells
jit = (xy + np.random.default_rng(1).uniform(-0.3, 0.3, xy.shape)).astype(np.float32)
nb = np.zeros((len(jit), 9), int)
for a in range(0, len(jit), 1000):
    dist = ((jit[a:a + 1000, None, :] - jit[None, :, :]) ** 2).sum(2)
    nb[a:a + 1000] = np.argpartition(dist, 9, axis=1)[:, :9]
    # the cell itself first
    for k in range(nb[a:a + 1000].shape[0]):
        row = nb[a + k]
        row[np.argmax(row == a + k)] = row[0]
        row[0] = a + k
same = (lab[nb[:, 1:]] == lab[:, None]).mean(1)
print(f"{'module':40s} cells  centre      RMS r  compact r  spread  same-module neighbours (share of design)")
rows = []
for m, n in mods.most_common(30):
    if m is None or n < 40:
        continue
    sel = lab == m
    p = xy[sel]
    r = math.sqrt(((p - p.mean(0)) ** 2).sum(1).mean())
    # RMS radius of a disc holding the cells at the density of the design's core (2 CPE halves per tile used ~70%)
    r0 = math.sqrt(n / 1.4 / math.pi) / math.sqrt(2)
    rows.append((m, n, p.mean(0), r, r0, same[sel].mean()))
    print(f"{m[:40]:40s} {n:5d}  ({p.mean(0)[0]:3.0f},{p.mean(0)[1]:3.0f})  {r:6.1f}  {r0:6.1f}  {r / r0:6.1f}  "
          f"{100 * same[sel].mean():3.0f}% ({100 * n / len(logic):.0f}%)")
print(f"all cells: {100 * same.mean():.0f}% of the 8 nearest cells are of the same module")
if png:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(13, 10))
    cmap = plt.get_cmap('tab20')
    names = [r[0] for r in rows[:19]]
    other = ~np.isin(lab, names)
    ax.scatter(xy[other, 0], xy[other, 1], s=3, c='#bbbbbb', label='other')
    for k, m in enumerate(names):
        sel = lab == m
        ax.scatter(xy[sel, 0], xy[sel, 1], s=4, color=cmap(k), label=f"{m[:28]} ({sel.sum()})")
    for c in cells:
        if c[2] in ('RAM', 'RAM_HALF'):
            ax.plot(c[0], c[1], 'ks', ms=5, mfc='none')
    ax.set_xlim(0, 165)
    ax.set_ylim(0, 135)
    ax.set_aspect('equal')
    ax.legend(loc='center left', bbox_to_anchor=(1.0, 0.5), fontsize=8, markerscale=3)
    ax.set_title(f)
    fig.tight_layout()
    fig.savefig(png, dpi=75)
