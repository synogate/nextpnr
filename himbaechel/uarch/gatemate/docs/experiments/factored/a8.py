import pickle
lat2=pickle.load(open("lat2.pkl","rb"))
def q(l,p): l=sorted(l); return l[int(p*(len(l)-1))]
U={1:(1,0),2:(0,1),3:(-1,0),4:(0,-1)}
# turn penalty: Y_i at S -> straight a along i -> corner C -> straight b along j ; compare with direct lattice value
for (i,j) in [(1,2),(1,4),(3,2),(2,1),(4,3)]:
    d=[];dc={}
    for (cx,cy,ci),L in lat2.items():
        if ci!=i: continue
        for a in range(4,25,2):
            for b in range(4,25,2):
                ux,uy=U[i]; vx,vy=U[j]
                C=(ux*a,uy*a); T=(C[0]+vx*b,C[1]+vy*b)
                l_direct=L.get((T[0],T[1],1,j))
                l_first=L.get((C[0],C[1],1,i))
                Lc=lat2.get(((cx+C[0])%4,(cy+C[1])%4,j))
                l_second=Lc.get((vx*b,vy*b,1,j)) if Lc else None
                if None in (l_direct,l_first,l_second): continue
                t=l_direct-l_first-l_second
                d.append(t)
    print("turn",i,"->",j,"n",len(d),"p10 %d med %d p90 %d"%(q(d,.1),q(d,.5),q(d,.9)))
