from dij import *
import sys
SX,SY=84,68
t=time.time()
src={node(tile(SX,SY),"CPE.OUT1_int"):0, node(tile(SX,SY),"CPE.OUT2_int"):0}
dist,pred=dijkstra(src,limit=9000)
print("dijkstra",time.time()-t,len(dist))
def best_in(x,y,pins=("IN1","IN2","IN3","IN4","IN5","IN6","IN7","IN8")):
    return {p:dist.get(node(tile(x,y),"CPE."+p),INF) for p in pins}
for name,(ux,uy) in [("east",(1,0)),("north",(0,1)),("west",(-1,0)),("south",(0,-1))]:
    row=[]
    for d in range(1,37):
        b=best_in(SX+ux*d,SY+uy*d)
        row.append(min(b.values()))
    print(name," ".join(f"{int(v) if v<INF else -1}" for v in row))
