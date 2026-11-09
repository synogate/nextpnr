import pickle
lat2=pickle.load(open("lat2.pkl","rb"))
def q(l,p): l=sorted(l); return l[int(p*(len(l)-1))]
U={1:(1,0),2:(0,1),3:(-1,0),4:(0,-1)}
for axis,name in ((0,"x"),(1,"y")):
  for sign in (1,-1):
    for P in (20,):
        d=[]
        for key,m in lat2.items():
            for (dx,dy,p,j),c in m.items():
                a=(dx,dy)[axis]*sign
                if not (P+1<=a<=36): continue
                b=(dx-sign*P,dy) if axis==0 else (dx,dy-sign*P)
                c0=m.get(b+(p,j))
                if c0 is not None: d.append(c-c0)
        print(name,"+" if sign>0 else "-","period",P,"n",len(d),"p5 %d p25 %d med %d p75 %d p95 %d"%(q(d,.05),q(d,.25),q(d,.5),q(d,.75),q(d,.95)))
