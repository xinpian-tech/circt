# cyccut.py tag IN OUT : tag the ops on combinational cycles with {cyc = k}
# cyccut.py cut IN OUT : every tagged value becomes an input cycin_k for its
#                        users and an output cyc_k (its driver), in order of k
import sys,re
sys.path.insert(0,'/tmp/claude-0/-root-yjh-workspace/3cca2a40-f56d-411e-8bea-05778e44e84e/scratchpad/wcg')
mode,src,dst=sys.argv[1:4]
t=open(src).read()
if mode=='tag':
    defs={};uses={};line={}
    L=t.split('\n')
    for i,l in enumerate(L):
        m=re.match(r'\s*(%[\w.$-]+) = (synth\.[\w.]+|comb\.\w+) (.*)',l)
        if not m: continue
        v=m.group(1); rhs=re.sub(r'loc\(.*','',m.group(3))
        defs[v]=1; uses[v]=[o for o in re.findall(r'%[\w.$-]+',rhs)]; line[v]=i
    # iterative tarjan
    index={};low={};st=[];on=set();k=0;n=0;cyc=[]
    for s in defs:
        if s in index: continue
        stack=[(s,0)]
        while stack:
            v,pi=stack.pop()
            if pi==0:
                index[v]=low[v]=n;n+=1;st.append(v);on.add(v)
            ch=[w for w in uses[v] if w in defs]
            if pi<len(ch):
                stack.append((v,pi+1)); w=ch[pi]
                if w not in index: stack.append((w,0))
                elif w in on: low[v]=min(low[v],index[w])
                continue
            if stack:
                p=stack[-1][0]
            for w in ch:
                if w in on: low[v]=min(low[v],low[w])
            if low[v]==index[v]:
                c=[]
                while True:
                    w=st.pop();on.discard(w);c.append(w)
                    if w==v: break
                if len(c)>1 or v in uses[v]: cyc+=c
    for k,v in enumerate(sorted(cyc,key=lambda v:line[v])):
        i=line[v]
        if re.search(r'\} : i1\s*(loc\(.*)?$', L[i]):
            L[i]=re.sub(r'\} : i1(\s*(loc\(.*)?)$', r', cyc = %d : i64} : i1\1' % k, L[i], count=1)
        else:
            L[i]=re.sub(r' : i1(\s*(loc\(.*)?)$', r' {cyc = %d : i64} : i1\1' % k, L[i], count=1)
    open(dst,'w').write('\n'.join(L)); print(len(cyc),'tagged',file=sys.stderr)
else:
    cuts=[]
    for m in re.finditer(r'\n\s*(%[\w.$-]+) = [^\n]*\{cyc = (\d+) : i64\}',t):
        cuts.append((int(m.group(2)),m.group(1)))
    cuts.sort()
    for k,v in cuts:
        t=re.sub(r'(\n\s*)'+re.escape(v)+r' = ', r'\g<1>%cycdrv'+str(k)+' = ', t, count=1)
        t=re.sub(re.escape(v)+r'(?![\w.$-])', '%cycin'+str(k), t)
    mi=re.search(r'hw\.module @\w+\(',t); p=mi.end()
    t=t[:p]+''.join(f'in %cycin{k} : i1, ' for k,v in cuts)+t[p:]
    he=t.index(') {',p)
    t=t[:he]+''.join(f', out cyc{k} : i1' for k,v in cuts)+t[he:]
    m=re.search(r'hw\.output (.*) : (.*)\n',t)
    t=t[:m.start()]+'hw.output '+m.group(1)+''.join(f', %cycdrv{k}' for k,v in cuts)+' : '+m.group(2)+', i1'*len(cuts)+'\n'+t[m.end():]
    open(dst,'w').write(t); print(len(cuts),'cut',file=sys.stderr)
