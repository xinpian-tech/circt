#!/usr/bin/env python3
"""Experiment: split the widest two-operand comb.mul of a module at bit K
into four partial products (exact, by distributivity modulo 2^W):
  x*y = xl*yl + ((xl*yh + xh*yl) << K) + (xh*yh << 2K)
with xl, yl the low K bits zero-extended and xh, yh the high bits
sign-extended. usage: splitmul.py in.mlir out.mlir K [MINWIDTH]"""
import re,sys
L=open(sys.argv[1]).read().split('\n'); K=int(sys.argv[3]); MINW=int(sys.argv[4]) if len(sys.argv)>4 else 48
n=0
for i,l in enumerate(L):
    m=re.match(r'(\s*)(%[\w.]+) = comb\.mul (%[\w.]+), (%[\w.]+)( \{[^}]*\})? : i(\d+)$',l)
    if not m or int(m[6])<MINW: continue
    ind,v,x,y,w=m[1],m[2],m[3],m[4],int(m[6]); p=f'%sp{n}'; n+=1; o=[]
    for nm,s in (('x',x),('y',y)):
        o+=[f'{p}{nm}l0 = comb.extract {s} from 0 : (i{w}) -> i{K}', f'{p}z = hw.constant 0 : i{w-K}' if nm=='x' else None,
            f'{p}{nm}l = comb.concat {p}z, {p}{nm}l0 : i{w-K}, i{K}',
            f'{p}{nm}h0 = comb.extract {s} from {K} : (i{w}) -> i{w-K}', f'{p}{nm}s = comb.extract {s} from {w-1} : (i{w}) -> i1',
            f'{p}{nm}r = comb.replicate {p}{nm}s : (i1) -> i{K}', f'{p}{nm}h = comb.concat {p}{nm}r, {p}{nm}h0 : i{K}, i{w-K}']
    o+=[f'{p}ll = comb.mul {p}xl, {p}yl : i{w}', f'{p}lh = comb.mul {p}xl, {p}yh : i{w}', f'{p}hl = comb.mul {p}xh, {p}yl : i{w}', f'{p}hh = comb.mul {p}xh, {p}yh : i{w}',
        f'{p}k = hw.constant {K} : i{w}', f'{p}k2 = hw.constant {2*K} : i{w}',
        f'{p}mid = comb.add {p}lh, {p}hl : i{w}', f'{p}mids = comb.shl {p}mid, {p}k : i{w}', f'{p}hhs = comb.shl {p}hh, {p}k2 : i{w}',
        f'{v} = comb.add {p}ll, {p}mids, {p}hhs : i{w}']
    L[i]='\n'.join(ind+t for t in o if t)
print('split',n,file=sys.stderr); open(sys.argv[2],'w').write('\n'.join(L))
