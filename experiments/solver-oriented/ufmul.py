#!/usr/bin/env python3
"""Upper-bound experiment: abstract every two-operand comb.mul of a flat
hw module as an uninterpreted function with a one-step history.
Each multiplier's result becomes a fresh input; assumptions state
functional consistency between multipliers of the same width in the same
cycle, and between each multiplier and each multiplier's operands and
result of the previous cycle (kept in registers). An over-approximation:
proofs carry over, counterexamples may be spurious.
usage: ufmul.py in.mlir out.mlir"""
import re,sys
L=open(sys.argv[1]).read().split('\n')
clk=re.search(r'in (%\w+) : !seq.clock',' '.join(L[:5]))[1]
muls=[]
for i,l in enumerate(L):
    m=re.match(r'(\s*)(%[\w.]+) = comb\.mul (%[\w.]+), (%[\w.]+)( \{[^}]*\})? : i(\d+)$',l)
    if m: muls.append((i,m[2],m[3],m[4],int(m[6]),m[1]))
new=[]; ports=[]
for k,(i,v,a,b,w,ind) in enumerate(muls):
    ports.append(f'in %uf{k} : i{w}')
    L[i]=f'{ind}{v} = comb.concat %uf{k} : i{w}'   # alias
    new+= [f'%ufha{k} = seq.compreg {a}, {clk} : i{w}', f'%ufhb{k} = seq.compreg {b}, {clk} : i{w}', f'%ufhr{k} = seq.compreg %uf{k}, {clk} : i{w}']
n=0
def low(v,w,wl,p):
    if w==wl: return v,[]
    return p,[f'{p} = comb.extract {v} from 0 : (i{w}) -> i{wl}']
def imp(a1,b1,r1,w1,a2,b2,r2,w2):
    # equal low operand bits give equal low result bits
    global n; n+=1; p=f'%ufc{n}'; w=min(w1,w2); o=[]; x=[]
    for i,(v,ww) in enumerate(((a1,w1),(b1,w1),(r1,w1),(a2,w2),(b2,w2),(r2,w2))):
        nv,ops=low(v,ww,w,f'{p}l{i}'); x.append(nv); o+=ops
    return o+[f'{p}a = comb.icmp eq {x[0]}, {x[3]} : i{w}', f'{p}b = comb.icmp eq {x[1]}, {x[4]} : i{w}', f'{p}r = comb.icmp eq {x[2]}, {x[5]} : i{w}',
            f'{p}n = comb.and {p}a, {p}b : i1', f'{p}t = hw.constant true', f'{p}x = comb.xor {p}n, {p}t : i1', f'{p}o = comb.or {p}x, {p}r : i1', f'verif.assume {p}o : i1']
for k,(i,v,a,b,w,ind) in enumerate(muls):
    for j,(i2,v2,a2,b2,w2,_) in enumerate(muls):
        if j>k: new+=imp(a,b,f'%uf{k}',w,a2,b2,f'%uf{j}',w2)+imp(a,b,f'%uf{k}',w,b2,a2,f'%uf{j}',w2)
        new+=imp(a,b,f'%uf{k}',w,f'%ufha{j}',f'%ufhb{j}',f'%ufhr{j}',w2)+imp(a,b,f'%uf{k}',w,f'%ufhb{j}',f'%ufha{j}',f'%ufhr{j}',w2)
# Range lemmas: an operand zero-extended from ka bits times one zero-extended
# from kb bits is below 2^(ka+kb); sign-extended from ka and kb bits, the
# product is sign-extended from ka+kb bits. Stated for a few widths.
KS=[int(x) for x in (sys.argv[3].split(',') if len(sys.argv)>3 else ['16','17','32','33'])]
def hi_zero(v,k,w,p): return [f'{p}e = comb.extract {v} from {k} : (i{w}) -> i{w-k}', f'{p}c = hw.constant 0 : i{w-k}', f'{p} = comb.icmp eq {p}e, {p}c : i{w-k}']
def hi_sext(v,k,w,p): return [f'{p}e = comb.extract {v} from {k-1} : (i{w}) -> i{w-k+1}', f'{p}c = hw.constant 0 : i{w-k+1}', f'{p}d = hw.constant -1 : i{w-k+1}',
    f'{p}z = comb.icmp eq {p}e, {p}c : i{w-k+1}', f'{p}o = comb.icmp eq {p}e, {p}d : i{w-k+1}', f'{p} = comb.or {p}z, {p}o : i1']
r=0
for k,(i,v,a_,b_,w,ind) in enumerate(muls):
    for ka in KS:
        for kb in KS:
            for kind,f in (('z',hi_zero),('s',hi_sext)):
                if ka>=w or kb>=w or ka+kb>=w: continue
                r+=1; p=f'%ufr{r}'
                new+=f(a_,ka,w,p+'a')+f(b_,kb,w,p+'b')+f(f'%uf{k}',ka+kb,w,p+'r')
                new+=[f'{p}n = comb.and {p}a, {p}b : i1', f'{p}t = hw.constant true', f'{p}x = comb.xor {p}n, {p}t : i1', f'{p}i = comb.or {p}x, {p}r : i1', f'verif.assume {p}i : i1']
print(r,'range lemmas',file=sys.stderr)
h=next(i for i,l in enumerate(L) if 'hw.module' in l)
L[h]=L[h].replace('(in ','('+', '.join(ports)+', in ',1)
o=next(i for i,l in enumerate(L) if 'hw.output' in l)
ind=re.match(r'\s*',L[o])[0]
L[o:o]=[ind+x for x in new]
open(sys.argv[2],'w').write('\n'.join(L)); print(len(muls),'multipliers abstracted,',n,'constraints',file=sys.stderr)
