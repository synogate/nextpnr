from dij import *
import re, pickle
lab=pickle.load(open("labels.pkl","rb"))
rx=re.compile(r'SB_(BIG|SML)\.P(\d\d)\.Y([1-4])$')
ynodes={}
for n,(t,w) in lab.items():
    m=rx.match(w)
    if m:
        i=t.index('Y'); x=int(t[1:i]); y=int(t[i+1:])
        ynodes[n]=(x,y,int(m.group(2)),int(m.group(3)),m.group(1))
bad=lambda v: (v in lab and lab[v][1][:3] in ("IM.","CPE","OM."))
out={}
for (SX,SY) in [(84,68),(86,68),(85,69),(87,69)]:
    typ=None
    for T in ("SB_BIG","SB_SML"):
        s=node(tile(SX,SY),f"{T}.P01.D0")
        if s is not None: typ=T; src=s
    t=time.time()
    dist,pred=dijkstra({src:0},skip=bad,limit=4000)
    m={}
    for n,(x,y,p,d,ty) in ynodes.items():
        if n in dist and abs(x-SX)<=40 and abs(y-SY)<=40:
            m[(x-SX,y-SY,p,d)]=(dist[n],ty)
    out[(SX,SY,typ)]=m
    print(SX,SY,typ,len(m),time.time()-t,flush=True)
pickle.dump((out,),open("e3.pkl","wb"))
