from dij import *
import pickle
PINS=["IN1","IN2","IN3","IN4","IN5","IN6","IN7","IN8","CLK","EN","SR"]
R=30
res={}
for (SX,SY) in [(84,68),(85,68),(84,69),(85,69),(84,70),(85,70),(84,71),(85,71)]:
    for o in ("OUT1","OUT2"):
        t=time.time()
        dist,pred=dijkstra({node(tile(SX,SY),f"CPE.{o}_int"):0},limit=7000)
        m={}
        for dx in range(-R,R+1):
            for dy in range(-R,R+1):
                m[(dx,dy)]=[dist.get(node(tile(SX+dx,SY+dy),"CPE."+p),INF) for p in PINS]
        res[(SX,SY,o)]=m
        print(SX,SY,o,time.time()-t,flush=True)
pickle.dump(res,open("e2.pkl","wb"))
