#!/usr/bin/env python3
"""Prototype of operator unsharing on flat CIRCT MLIR text (hw/comb).
For every comb.mul, Shannon-expand over the selects of the muxes found in
its operand cones (walking through mux/concat/replicate/extract only):
  mul(mux(s,a,c), mux(s,b,d)) -> mux(s, mul(a,b), mul(c,d))
so that each resulting multiplier has mux-free operands and can be merged
with an identical multiplier elsewhere (the property side) by
canonicalize+cse. Exact (no abstraction).
usage: unshare.py in.mlir out.mlir [maxleaves=32] [maxsrc=64]"""
import re,sys
src=open(sys.argv[1]).read().split('\n')
MAXL=int(sys.argv[3]) if len(sys.argv)>3 else 32
MAXS=int(sys.argv[4]) if len(sys.argv)>4 else 64
D={}
for i,l in enumerate(src):
    m=re.match(r'(\s*)(%[\w.]+) = (comb\.\w+)( bin)? (.*)$',l)
    if m: D[m[2]]=(i,m[3],m[5],m[1])
V=re.compile(r'%[\w.]+')
def ops(v):
    r=D[v][2]; return V.findall(r.split(' : ')[0].split(' {')[0])
def walk(v):  # may we look through v?
    if v not in D: return False
    op=D[v][1]
    if op in ('comb.mux','comb.concat','comb.replicate'): return True
    if op=='comb.extract':
        w=int(re.search(r'\(i(\d+)\)',D[v][2])[1]); return w<=MAXS
    return False
def first_sel(v,env,seen):
    if v in seen or not walk(v): return None
    seen.add(v); o=ops(v)
    if D[v][1]=='comb.mux':
        if o[0] in env: return first_sel(o[1] if env[o[0]] else o[2],env,seen)
        return o[0]
    for u in o:
        s=first_sel(u,env,seen)
        if s: return s
    return None
out=[]; memo={}; cnt=[0]
def fresh(): cnt[0]+=1; return f'%us{cnt[0]}'
def has_assigned(v,env,seen):
    if v in seen or not walk(v): return False
    seen.add(v); o=ops(v)
    if D[v][1]=='comb.mux' and o[0] in env: return True
    return any(has_assigned(u,env,seen) for u in o)
def clone(v,env):
    if not has_assigned(v,env,set()): return v
    k=(v,tuple(sorted(env.items())))
    if k in memo: return memo[k]
    o=ops(v); op,rhs=D[v][1],D[v][2]
    if op=='comb.mux' and o[0] in env:
        r=clone(o[1] if env[o[0]] else o[2],env)
    else:
        sub={u:clone(u,env) for u in o}; r=fresh()
        head,sep,ty=rhs.rpartition(' : ')
        head=head.split(' {')[0]
        out.append(f'{r} = {op} '+V.sub(lambda m:sub.get(m[0],m[0]),head)+sep+ty)
    memo[k]=r; return r
def build(mv,env,budget):
    a,b=ops(mv); ty=D[mv][2].rsplit(' : ',1)[1]
    s=first_sel(a,env,set()) or first_sel(b,env,set())
    if s is None or budget[0]<=0:
        budget[0]-=1; r=fresh()
        out.append(f'{r} = comb.mul {clone(a,env)}, {clone(b,env)} : {ty}'); return r
    t=build(mv,{**env,s:True},budget); f=build(mv,{**env,s:False},budget)
    r=fresh(); out.append(f'{r} = comb.mux {s}, {t}, {f} : {ty}'); return r
n=0
for v,(i,op,rhs,ind) in list(D.items()):
    if op!='comb.mul': continue
    a,b=ops(v)
    if not (first_sel(a,{},set()) or first_sel(b,{},set())): continue
    out=[]; memo={}; bud=[MAXL]; r=build(v,{},bud)
    if bud[0]<0: print('skip (too many leaves)',v,file=sys.stderr); continue
    nm=sum(' comb.mul ' in l for l in out)
    out[-1]=out[-1].replace(r+' =',v+' =',1)
    src[i]='\n'.join(ind+l for l in out); n+=1
    print(f'unshared {v}: {nm} multipliers, {len(out)} ops',file=sys.stderr)
open(sys.argv[2],'w').write('\n'.join(src))
