import pickle, random, statistics as st, sys
R0=int(sys.argv[3]) if len(sys.argv)>3 else 8
exec(open("proto_eval.py").read().split("random.seed(1)")[0].replace("def h(","def h_old("))
lat2=pickle.load(open("lat2.pkl","rb"))
RING=[0,214,437,666,886,1117,1314]
def h(sk,tx,ty,pin):
    X=100+(tx-100)%4; Y=80+(ty-80)%4; sx,sy=tx-X,ty-Y
    best=INF
    for (ex_,ey,p,i),ce in ex[sk]:
        L2=lat2[(ex_%4,ey%4,i)]; L1=lat[(ex_%4,ey%4,i)]
        for (nx,ny,q,j),cn in en[(X,Y,pin)]:
            ddx,ddy=nx+sx-ex_,ny+sy-ey
            if max(abs(ddx),abs(ddy))<=R0:
                l=L2.get((ddx,ddy,(q-p)%12+1,j))
            else:
                l=L1.get((ddx,ddy,j)); l=None if l is None else l+RING[ring(p,q)]
            if l is None: continue
            v=ce+l+cn
            if v<best: best=v
    return best,None
exec("random.seed(1)"+open("proto_eval.py").read().split("random.seed(1)")[1])
