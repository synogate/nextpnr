import re
from collections import defaultdict
cls=defaultdict(list)
def norm(w):
    w=w.split('/',1)[1]
    w=re.sub(r'\.P\d\d\.','.Pp.',w)
    return w
with open("pips.tsv") as f:
    for line in f:
        a,b,d,_=line.split('\t')
        xy=a.split('/')[0]; i=xy.index('Y'); x=int(xy[1:i]); y=int(xy[i+1:])
        if not (72<=x<88 and 60<=y<76): continue
        bx=b.split('/')[0]
        k=(norm(a),norm(b), 'same' if bx==xy else 'other')
        cls[k].append(int(d))
rows=[]
for k,v in cls.items():
    v.sort(); rows.append((k,len(v),v[0],v[len(v)//2],v[-1]))
for k,n,mn,md,mx in sorted(rows):
    print(f"{k[0]:32s} {k[1]:32s} {k[2]:5s} n={n:6d} min={mn:5d} med={md:5d} max={mx:5d}")
