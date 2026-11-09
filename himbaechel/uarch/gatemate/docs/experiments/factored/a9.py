# Test: per (source CPE class, target CPE class in its 4x4 tile) axis tables + turn (+ plane)
import pickle, statistics as st
INF=float('inf')
res=pickle.load(open("e2.pkl","rb"))
def q(l,p): l=sorted(l); return l[int(p*(len(l)-1))]
def rep(name,errs):
    a=[abs(e) for e in errs]
    print(f"{name:55s} mae {st.mean(a):4.0f} mean {st.mean(errs):+4.0f} p95|e| {q(a,.95):4.0f} within150 {100*sum(x<=150 for x in a)/len(a):3.0f}%  n={len(a)}")
def tiles(key,dx,dy):
    sx,sy=key[0],key[1]; tx,ty=sx+dx,sy+dy
    return (tx%4,ty%4),(tx//4-sx//4),(ty//4-sy//4)
# per pin, per class: T[(dcls,DX,DY)]
for variant in ("per-pin tables","min-over-pins tables + plane table"):
    errs=[];errs_band={}
    turns=[]
    for key,m in res.items():
        for pin in range(11):
            T={}; Tmin={}
            for (dx,dy),v in m.items():
                if max(abs(dx),abs(dy))==0: continue
                d,DX,DY=tiles(key,dx,dy)
                T[(d,DX,DY)]=v[pin]; Tmin[(d,DX,DY)]=min(v[:8]) if pin<8 else v[pin]
            src=T if variant.startswith("per-pin") else Tmin
            # plane table: distance-independent extra of this pin over the best pin, per target class
            pl={}
            if not variant.startswith("per-pin"):
                for (d,DX,DY),v in T.items():
                    pl.setdefault(d,[]).append(v-Tmin[(d,DX,DY)])
                pl={d:st.median(l) for d,l in pl.items()}
            for (d,DX,DY),v in T.items():
                if DX==0 or DY==0 or v==INF: continue
                a=src.get((d,DX,0)); b=src.get((d,0,DY)); c=src.get((d,0,0))
                if None in (a,b,c) or INF in (a,b,c): continue
                pred=a+b-c+(pl.get(d,0))
                turns.append(v-pred)
    tc=st.median(turns)
    print(variant,"turn constant (median residual) =",round(tc))
    rep(variant,[t-tc for t in turns])
