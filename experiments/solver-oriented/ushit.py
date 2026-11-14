#!/usr/bin/env python3
"""Decide whether unsharing pays: in the canonicalized, CSE'd output of
unshare.py (leaf multipliers carry the name hint "usleaf"), report the
leaves whose operands equal those of a multiplier that was already in
the model, also when the leaf is wider and its operands are extensions
of that multiplier's (the low bits of a product depend only on the low
bits of the operands). Exit 0 if there is a hit.
usage: ushit.py canon.mlir"""
import re,sys
D={}; muls=[]
for l in open(sys.argv[1]):
    m=re.match(r'\s*(%[\w.]+) = (comb\.\w+)( bin)? (.*)$',l)
    if not m: continue
    D[m[1]]=(m[2],m[4])
    if m[2]=='comb.mul': muls.append(m[1])
V=re.compile(r'%[\w.]+')
def ops(v): return V.findall(D[v][1].split(' : ')[0].split(' {')[0])
def width(v): return int(D[v][1].rsplit(' : i',1)[1])
def low(v):  # the value v extends, if v = concat(..., x)
    if v in D and D[v][0]=='comb.concat': return ops(v)[-1]
    return None
leaf=[m for m in muls if 'usleaf' in D[m][1]]; old=[m for m in muls if m not in leaf]
hits=[]
for L in leaf:
    for E in old:
        a,b=ops(L); x,y=ops(E)
        for p,q in ((a,b),(b,a)):
            if (p==x or low(p)==x) and (q==y or low(q)==y) and width(E)<=width(L):
                hits.append((L,E)); break
print(f'{len(leaf)} leaves, {len(old)} other multipliers, hits: {hits}')
sys.exit(0 if hits else 1)
