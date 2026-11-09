import time
t=time.time()
f=open("pips.tsv","w")
n=0
for p in ctx.getPips():
    s=str(p)
    # keep the central window only
    xy=s.split('/',1)[0]
    i=xy.index('Y'); x=int(xy[1:i]); y=int(xy[i+1:])
    if not (30<=x<=140 and 30<=y<=110): continue
    d=ctx.getPipDelay(p).maxDelay()
    f.write(f"{ctx.getPipSrcWire(p)}\t{ctx.getPipDstWire(p)}\t{d}\t{s}\n")
    n+=1
f.close()
print("pips",n,time.time()-t)
