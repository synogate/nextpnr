import pickle, random, statistics as st, sys
exits,entries,lat=pickle.load(open("proto.pkl","rb"))
res=pickle.load(open("e2.pkl","rb"))
PINS=["IN1","IN2","IN3","IN4","IN5","IN6","IN7","IN8","CLK","EN","SR"]
PSTEP=int(sys.argv[1]) if len(sys.argv)>1 else 220
PR=int(sys.argv[2]) if len(sys.argv)>2 else 600
def ring(p,q): d=abs(p-q); return min(d,12-d)
def prune(l):
    m=min(c for _,c in l); return [(a,c) for a,c in l if c<=m+PR]
ex={k:prune(v) for k,v in exits.items()}
en={k:prune(v) for k,v in entries.items()}
INF=float('inf')
def h(sk,tx,ty,pin):
    X=100+(tx-100)%4; Y=80+(ty-80)%4; sx,sy=tx-X,ty-Y
    best=INF; arg=None
    for (ex_,ey,p,i),ce in ex[sk]:
        L=lat[(ex_%4,ey%4,i)]
        for (nx,ny,q,j),cn in en[(X,Y,pin)]:
            l=L.get((nx+sx-ex_,ny+sy-ey,j))
            if l is None: continue
            v=ce+l+PSTEP*ring(p,q)+cn
            if v<best: best=v; arg=((ex_,ey,p,i),(nx+sx,ny+sy,q,j))
    return best,arg
random.seed(1)
keys=list(res.keys())
samples=[]
while len(samples)<6000:
    k=random.choice(keys); dx=random.randint(-30,30); dy=random.randint(-30,30)
    if max(abs(dx),abs(dy))<1: continue
    pi=random.randrange(11)
    t=res[k][(dx,dy)][pi]
    if t==INF: continue
    samples.append((k,dx,dy,pi,t))
bands=[(1,1),(2,3),(4,7),(8,15),(16,31)]
err={b:[] for b in bands}; allerr=[]
for k,dx,dy,pi,t in samples:
    v,_=h((k[0],k[1],k[2]),k[0]+dx,k[1]+dy,PINS[pi])
    e=v-t; allerr.append(e)
    md=abs(dx)+abs(dy)
    for b in bands:
        if b[0]<=md<=b[1] or (b==(16,31) and md>=16): err[b].append(e)
f=lambda l: "mae %4.0f mean %+4.0f  within150 %3.0f%%  n=%d"%(st.mean(abs(x) for x in l),st.mean(l),100*sum(abs(x)<=150 for x in l)/len(l),len(l))
for b in bands: print(b,f(err[b]))
print("all",f(allerr))
neg=sorted(allerr)[:5]; print("most optimistic",neg, "most pessimistic", sorted(allerr)[-5:])
