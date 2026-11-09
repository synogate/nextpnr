import pickle, random, statistics as st, sys
exec(open("proto_eval.py").read().split("random.seed(1)")[0].replace("def h(","def h_old("))
lat2=pickle.load(open("lat2.pkl","rb"))
# check: plane additivity in the lattice: lat2[(dx,dy,q,j)] - lat2[(dx,dy,1,j)] vs 220*ring
diffs={}
for key,m in lat2.items():
    for (dx,dy,q,j),c in m.items():
        b=m.get((dx,dy,1,j))
        if b is None or max(abs(dx),abs(dy))<6: continue
        diffs.setdefault((ring(1,q), dx==0 or dy==0),[]).append(c-b)
for k in sorted(diffs):
    l=sorted(diffs[k]); print("ring",k[0],"straight" if k[1] else "2D","p10 %d med %d p90 %d"%(l[len(l)//10],l[len(l)//2],l[9*len(l)//10]))
def h(sk,tx,ty,pin):
    X=100+(tx-100)%4; Y=80+(ty-80)%4; sx,sy=tx-X,ty-Y
    best=INF
    for (ex_,ey,p,i),ce in ex[sk]:
        L=lat2[(ex_%4,ey%4,i)]
        for (nx,ny,q,j),cn in en[(X,Y,pin)]:
            r=(q-p)%12+1
            l=L.get((nx+sx-ex_,ny+sy-ey,r,j))
            if l is None: continue
            v=ce+l+cn
            if v<best: best=v
    return best,None
exec("random.seed(1)"+open("proto_eval.py").read().split("random.seed(1)")[1])
