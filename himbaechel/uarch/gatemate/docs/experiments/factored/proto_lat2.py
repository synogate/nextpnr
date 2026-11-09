from dij import *
import re, pickle
lab=pickle.load(open("labels.pkl","rb"))
rx=re.compile(r'SB_(BIG|SML)\.P(\d\d)\.Y([1-4])$')
ynode={}
for n,(t,w) in lab.items():
    m=rx.match(w)
    if m:
        i=t.index('Y'); ynode[n]=(int(t[1:i]),int(t[i+1:]),int(m.group(2)),int(m.group(3)))
isfab=lambda n: n in lab and lab[n][1][:3] in ("IM.","CPE","OM.")
lat={}
for X in range(88,92):
    for Y in range(72,76):
        if (X-Y)%2: continue
        for i in range(1,5):
            s=[n for n,v in ynode.items() if v==(X,Y,1,i)][0]
            dist,_=dijkstra({s:0},skip=isfab,limit=5000)
            m={}
            for n,c in dist.items():
                if n in ynode:
                    x,y,p,j=ynode[n]
                    if abs(x-X)<=36 and abs(y-Y)<=36: m[(x-X,y-Y,p,j)]=c
            lat[(X%4,Y%4,i)]=m
        print("lat",X,Y,flush=True)
pickle.dump(lat,open("lat2.pkl","wb"))
