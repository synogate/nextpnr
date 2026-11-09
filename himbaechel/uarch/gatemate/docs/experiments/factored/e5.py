from dij import *
SX,SY=84,68
dist,pred=dijkstra({node(tile(SX,SY),"CPE.OUT1_int"):0,node(tile(SX,SY),"CPE.OUT2_int"):0},limit=3000)
pairs=[("TI2_int","IN2"),("TI4_int","IN4"),("TI6_int","IN6"),("TI8_int","IN8"),("CLK_int","CLK"),("EN_int","EN")]
print("cells: best(TI-like) / fabric-only(IN) ; '*' = CP line wins")
for ti,inn in pairs:
    print("==",ti)
    for dy in range(10,-1,-1):
        row=[]
        for dx in range(0,9):
            a=dist.get(node(tile(SX+dx,SY+dy),"CPE."+ti),INF); b=dist.get(node(tile(SX+dx,SY+dy),"CPE."+inn),INF)
            row.append(f"{int(a):5d}{'*' if a<b-1 else ' '}" if a<INF else "   -  ")
        print(f"dy={dy:2d}"," ".join(row))
# show a few paths
for (dx,dy,w) in [(0,3,"TI2_int"),(3,0,"TI8_int"),(2,3,"TI2_int"),(0,6,"CLK_int")]:
    n=node(tile(SX+dx,SY+dy),"CPE."+w); print(dx,dy,w,int(dist[n]),[x.split('/',1)[1] if '/' in x else x for x in path(pred,n)])
