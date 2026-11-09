# Build the three components from the real graph
from dij import *
import re, pickle, numpy as np
lab=pickle.load(open("labels.pkl","rb"))
rx=re.compile(r'SB_(BIG|SML)\.P(\d\d)\.Y([1-4])$')
ynode={}
for n,(t,w) in lab.items():
    m=rx.match(w)
    if m:
        i=t.index('Y'); ynode[n]=(int(t[1:i]),int(t[i+1:]),int(m.group(2)),int(m.group(3)))
def pl(n):
    if n not in lab: return None
    m=re.search(r'\.P(\d\d)\.',lab[n][1]); return int(m.group(1)) if m else None
isfab=lambda n: n in lab and lab[n][1][:3] in ("IM.","CPE","OM.")
# reverse CSR
Dn=np.array(D,dtype=np.int64); Wn=np.array(W); S=np.repeat(np.arange(len(start)-1),np.diff(np.array(start)))
o=np.argsort(Dn,kind='stable'); RS=S[o].tolist(); RW=Wn[o].tolist()
rstart=np.searchsorted(Dn[o],np.arange(len(start))).tolist()
def dij_term(src, fwd=True, limit=2500):
    """Dijkstra where Y nodes are terminal; returns {ynode: cost}"""
    dist={}; h=[(c,s) for s,c in src.items()]; heapq.heapify(h); got={}
    st_,dd,ww=(start,D,W) if fwd else (rstart,RS,RW)
    while h:
        c,u=heapq.heappop(h)
        if u in dist: continue
        dist[u]=c
        if c>limit: break
        if u in ynode and u not in src:
            got[u]=c; continue
        for e in range(st_[u],st_[u+1]):
            v=dd[e]
            if v not in dist: heapq.heappush(h,(c+ww[e],v))
    return got
PINS=["IN1","IN2","IN3","IN4","IN5","IN6","IN7","IN8","CLK","EN","SR"]
# exits for the e2 sources
exits={}
for (SX,SY) in [(84,68),(85,68),(84,69),(85,69),(84,70),(85,70),(84,71),(85,71)]:
    for o_ in ("OUT1","OUT2"):
        g=dij_term({node(tile(SX,SY),f"CPE.{o_}_int"):0},True,1500)
        exits[(SX,SY,o_)]=[(ynode[n],c) for n,c in g.items()]
print("exits", {k:len(v) for k,v in list(exits.items())[:3]},flush=True)
# entries for target CPEs in a 8x8 block of phases (translate later): all classes x mod 2,(x+y) mod 4 -> use 4x4 block at 100..103, 80..83
entries={}
for X in range(100,104):
    for Y in range(80,84):
        for p in PINS:
            g=dij_term({node(tile(X,Y),"CPE."+p):0},False,1500)
            entries[(X,Y,p)]=[(ynode[n],c) for n,c in g.items()]
print("entries",len(entries),flush=True)
# lattice: from Y_i of SBs of every class (x mod 4, y mod 4 with x=y mod 2) on plane 1, SB-only plane-1 nodes
lat={}
for X in range(88,92):
    for Y in range(72,76):
        if (X-Y)%2: continue
        for i in range(1,5):
            s=[n for n,v in ynode.items() if v==(X,Y,1,i)]
            if not s: print("no",X,Y,i); continue
            dist,_=dijkstra({s[0]:0},skip=lambda v: isfab(v) or (pl(v) not in (None,1)),limit=4000)
            m={}
            for n,c in dist.items():
                if n in ynode:
                    x,y,p,j=ynode[n]
                    if p==1: m[(x-X,y-Y,j)]=c
            lat[(X%4,Y%4,i)]=m
        print("lat",X,Y,flush=True)
pickle.dump((exits,entries,lat),open("proto.pkl","wb"))
