# Best possible additive model: h = A[src,out,pin,dcls] + X[scls,dcls,DX] + Y[scls,dcls,DY] + turn*[DX!=0 and DY!=0]
# fitted by backfitting with medians (L1), to see what the structure can reach at all
import pickle, statistics as st, sys
from collections import defaultdict
INF=float('inf')
res=pickle.load(open("e2.pkl","rb"))
mode=sys.argv[1] if len(sys.argv)>1 else "full"
rows=[]
for key,m in res.items():
    sx,sy,o=key
    for (dx,dy),v in m.items():
        if dx==0 and dy==0: continue
        tx,ty=sx+dx,sy+dy; d=(tx%4,ty%4)
        DX=tx//4-sx//4; DY=ty//4-sy//4
        s=(sx%4,sy%4)
        for pin in range(11):
            if v[pin]==INF: continue
            if mode in ("full","quad"): kx=(s,d,DX); ky=(s,d,DY)
            elif mode=="axisclass": kx=(sx%4,tx%4,sy%4,DX); ky=(sy%4,ty%4,sx%4,DY)   # x table knows the row class of the source only
            sg=lambda a:(a>0)-(a<0)
            akey=(sx,sy,o,pin,d) if mode!="quad" else (sx,sy,o,pin,d,sg(DX),sg(DY))
            rows.append((akey,kx,ky,1 if (DX!=0 and DY!=0) else 0,v[pin],abs(dx)+abs(dy)))
print("rows",len(rows))
A=defaultdict(float);X=defaultdict(float);Y=defaultdict(float);turn=0.0
def med_update(keyf, resid):
    g=defaultdict(list)
    for r in rows: g[keyf(r)].append(resid(r))
    return {k:st.median(l) for k,l in g.items()}
for it in range(6):
    A=med_update(lambda r:r[0], lambda r:r[4]-X[r[1]]-Y[r[2]]-turn*r[3])
    X=defaultdict(float,med_update(lambda r:r[1], lambda r:r[4]-A[r[0]]-Y[r[2]]-turn*r[3]))
    Y=defaultdict(float,med_update(lambda r:r[2], lambda r:r[4]-A[r[0]]-X[r[1]]-turn*r[3]))
    tr=[r[4]-A[r[0]]-X[r[1]]-Y[r[2]] for r in rows if r[3]]
    turn=st.median(tr)
    errs=[r[4]-A[r[0]]-X[r[1]]-Y[r[2]]-turn*r[3] for r in rows]
    a=[abs(e) for e in errs]
    print(mode,"iter",it,"turn",round(turn),"mae %.0f within150 %.0f%%"%(st.mean(a),100*sum(x<=150 for x in a)/len(a)),flush=True)
bands=[(1,1),(2,3),(4,7),(8,15),(16,60)]
for b in bands:
    e=[abs(r[4]-A[r[0]]-X[r[1]]-Y[r[2]]-turn*r[3]) for r in rows if b[0]<=r[5]<=b[1]]
    print(" band",b,"mae %.0f"%st.mean(e))
print("params A",len(A),"X",len(X),"Y",len(Y))
