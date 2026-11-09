import numpy as np, pickle, time, sys
t=time.time()
ids={}; names=[]
S=[];D=[];W=[]
def gid(n):
    i=ids.get(n)
    if i is None:
        i=len(names); ids[n]=i; names.append(n)
    return i
with open("pips.tsv") as f:
    for line in f:
        a,b,d,_=line.split('\t')
        S.append(gid(a)); D.append(gid(b)); W.append(int(d))
S=np.array(S,dtype=np.int32); D=np.array(D,dtype=np.int32); W=np.array(W,dtype=np.int32)
o=np.argsort(S,kind='stable')
S=S[o];D=D[o];W=W[o]
start=np.searchsorted(S,np.arange(len(names)+1))
pickle.dump((names,start,D,W),open("graph.pkl","wb"),protocol=4)
print(len(names),len(D),time.time()-t)
