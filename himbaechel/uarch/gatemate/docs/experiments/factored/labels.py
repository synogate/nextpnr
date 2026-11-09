import pickle
ids={}
names,start,D,W=pickle.load(open("graph.pkl","rb"))
ids={n:i for i,n in enumerate(names)}
lab={}
with open("pips.tsv") as f:
    for line in f:
        a,b,d,pn=line.rstrip('\n').split('\t')
        t,dst,src=pn.split('/',2)
        lab.setdefault(ids[b],(t,dst))
pickle.dump(lab,open("labels.pkl","wb"),protocol=4)
print(len(lab))
