exec(open("proto_lat2.py").read().split("lat={}")[0])
def q(l,p): l=sorted(l); return l[int(p*(len(l)-1))]
for (X,Y,i) in [(88,72,1),(88,72,3),(89,73,1),(89,73,3)]:
    s=[n for n,v in ynode.items() if v==(X,Y,1,i)][0]
    dist,_=dijkstra({s:0},skip=isfab,limit=9000)
    m={}
    for n,c in dist.items():
        if n in ynode:
            x,y,p,j=ynode[n]; m[(x-X,y-Y,p,j)]=c
    for sign in (1,-1):
        for lo,hi in ((36,52),(44,52)):
            d=[]
            for (dx,dy,p,j),c in m.items():
                a=dx*sign
                if lo<=a<=hi and abs(dy)<=30:
                    c0=m.get((dx-sign*20,dy,p,j))
                    if c0 is not None: d.append(c-c0)
            if d: print((X,Y,i),"x","+" if sign>0 else "-",(lo,hi),"n",len(d),"p5 %d p25 %d med %d p75 %d p95 %d"%(q(d,.05),q(d,.25),q(d,.5),q(d,.75),q(d,.95)),flush=True)
