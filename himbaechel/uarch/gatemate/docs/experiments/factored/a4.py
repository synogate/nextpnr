import pickle, statistics as st
(out,)=pickle.load(open("e3.pkl","rb"))
def q(x,p): x=sorted(x); return x[int(p*(len(x)-1))]
U={1:(1,0),2:(0,1),3:(-1,0),4:(0,-1)}
for key,m in out.items():
    print("==",key)
    # Y_dir at SB on the axis line: cost of being at SB (d*u) leaving in direction dir (same plane 1)
    for d_ in (1,2,3,4):
        ux,uy=U[d_]
        row=[]
        for k in range(0,41,2):
            v=m.get((ux*k,uy*k,1,d_))
            row.append(int(v[0]) if v else -1)
        print(" straight dir",d_," ".join(map(str,row)))
    # plane cost: at |pos|>=6, cost(q)-cost(1) for same pos/dir
    for qq in range(2,13):
        r=[m[(dx,dy,qq,d)][0]-m[(dx,dy,1,d)][0] for (dx,dy,p,d) in m if p==1 and (dx,dy,qq,d) in m and max(abs(dx),abs(dy))>=6]
        print(" plane",qq,"p10 %d med %d p90 %d"%(q(r,.1),q(r,.5),q(r,.9)))
    break
