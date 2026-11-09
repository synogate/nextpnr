import pickle, heapq, time
names,start,D,W=pickle.load(open("graph.pkl","rb"))
start=start.tolist(); D=D.tolist(); W=W.tolist()
ids={n:i for i,n in enumerate(names)}
INF=float('inf')
def dijkstra(src, skip=None, limit=None):
    """src: dict node->cost. returns dist dict-like list and pred"""
    dist={}; pred={}
    h=[(c,s,-1) for s,c in src.items()]
    heapq.heapify(h)
    while h:
        c,u,p=heapq.heappop(h)
        if u in dist: continue
        dist[u]=c; pred[u]=p
        if limit and c>limit: break
        for e in range(start[u],start[u+1]):
            v=D[e]
            if v in dist: continue
            if skip and skip(v): continue
            heapq.heappush(h,(c+W[e],v,u))
    return dist,pred
def node(t,w): return ids.get(f"{t}/{w}")
def tile(x,y): return f"X{x}Y{y}"
def path(pred,v):
    r=[]
    while v!=-1:
        r.append(names[v]); v=pred[v]
    return r[::-1]
