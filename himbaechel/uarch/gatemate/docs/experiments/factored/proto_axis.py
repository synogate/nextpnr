# Axis-table variant: exit list + 1D run tables (per SB class, direction, length) + turn at the corner
# + plane ring + entry list. No 2D lattice table.
import pickle, random, statistics as st, sys
EXTRA=int(sys.argv[1]) if len(sys.argv)>1 else 0     # plane change on a route without a turn
PR=600
exits,entries,lat=pickle.load(open("proto.pkl","rb"))
res=pickle.load(open("e2.pkl","rb"))
PINS=["IN1","IN2","IN3","IN4","IN5","IN6","IN7","IN8","CLK","EN","SR"]
U={1:(1,0),2:(0,1),3:(-1,0),4:(0,-1)}
RING=[0,214,437,666,886,1117,1314]
def ring(p,q): d=abs(p-q); return min(d,12-d)
def prune(l):
    m=min(c for _,c in l); return [(a,c) for a,c in l if c<=m+PR]
ex={k:prune(v) for k,v in exits.items()}
en={k:prune(v) for k,v in entries.items()}
INF=float('inf')
# 1D run tables (per SB class, direction, even length) read off the plane-1 lattice, and the corner
# table: from arriving on Yi at corner C to leaving on Yj at C + o (o = 0 or a diagonal step +-(1,1))
RUN={}; TC={}
OFFS=[(0,0),(1,1),(-1,-1)]
for (cx,cy,i),L in lat.items():
    ux,uy=U[i]
    RUN[(cx,cy,i)]=[L.get((ux*a,uy*a,i),INF) if a else 0 for a in range(0,41)]
    for a in range(4,21,2):
        C=(ux*a,uy*a); base=L.get(C+(i,))
        if base is None: continue
        cc=((cx+C[0])%4,(cy+C[1])%4)
        for j in range(1,5):
            if j==i: continue
            for o in OFFS:
                v=L.get((C[0]+o[0],C[1]+o[1],j))
                if v is not None: TC.setdefault((cc,i,j,o),[]).append(v-base)
TURN={k:sorted(v)[len(v)//2] for k,v in TC.items()}
size=sum(len(v) for v in RUN.values())+len(TURN)
def run(x,y,i,a):
    if a<0 or a%2: return INF
    r=RUN[(x%4,y%4,i)]
    return r[a] if a<len(r) else INF
def h(sk,tx,ty,pin):
    X=100+(tx-100)%4; Y=80+(ty-80)%4; sx,sy=tx-X,ty-Y
    best=INF
    for (x0,y0,p,i),ce in ex[sk]:
        ux,uy=U[i]
        for (nx,ny,q,j),cn in en[(X,Y,pin)]:
            x1,y1=nx+sx,ny+sy; dx,dy=x1-x0,y1-y0
            vx,vy=U[j]
            a=dx*ux+dy*uy
            if i==j:
                if dx*uy-dy*ux!=0: continue
                c=run(x0,y0,i,a); turned=False
            elif ux*vx+uy*vy==0:
                c=INF
                for o in OFFS:
                    ddx,ddy=dx-o[0],dy-o[1]
                    a=ddx*ux+ddy*uy; b=ddx*vx+ddy*vy
                    if a<0 or b<0 or a%2 or b%2: continue
                    cxr,cyr=x0+ux*a,y0+uy*a
                    t=TURN.get(((cxr%4,cyr%4),i,j,o))
                    if t is None: continue
                    c=min(c,run(x0,y0,i,a)+t+run(cxr+o[0],cyr+o[1],j,b))
                turned=True
            else: continue
            r=ring(p,q)
            v=ce+c+RING[r]+(EXTRA if r and not turned else 0)+cn
            if v<best: best=v
    return best
random.seed(1)
keys=list(res.keys()); samples=[]
while len(samples)<6000:
    k=random.choice(keys); dx=random.randint(-30,30); dy=random.randint(-30,30)
    if max(abs(dx),abs(dy))<1: continue
    pi=random.randrange(11); t=res[k][(dx,dy)][pi]
    if t==INF: continue
    samples.append((k,dx,dy,pi,t))
bands=[(1,1),(2,3),(4,7),(8,15),(16,60)]
err={b:[] for b in bands}; allerr=[]
for k,dx,dy,pi,t in samples:
    v=h(k,k[0]+dx,k[1]+dy,PINS[pi]); e=v-t; allerr.append(e)
    md=abs(dx)+abs(dy)
    for b in bands:
        if b[0]<=md<=b[1]: err[b].append(e)
f=lambda l: "mae %4.0f mean %+4.0f within150 %3.0f%% n=%d"%(st.mean(abs(x) for x in l),st.mean(l),100*sum(abs(x)<=150 for x in l)/len(l),len(l))
print("EXTRA",EXTRA,"table entries",size)
for b in bands: print(b,f(err[b]))
print("all",f(allerr)); s=sorted(allerr); print("extremes",s[:3],s[-3:])
