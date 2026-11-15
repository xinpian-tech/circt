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
def imp(a1,b1,r1,a2,b2,r2,w):
    global n; n+=1; p=f'%ufc{n}'
    return [f'{p}a = comb.icmp eq {a1}, {a2} : i{w}', f'{p}b = comb.icmp eq {b1}, {b2} : i{w}', f'{p}r = comb.icmp eq {r1}, {r2} : i{w}',
            f'{p}n = comb.and {p}a, {p}b : i1', f'{p}t = hw.constant true', f'{p}x = comb.xor {p}n, {p}t : i1', f'{p}o = comb.or {p}x, {p}r : i1', f'verif.assume {p}o : i1']
for k,(i,v,a,b,w,ind) in enumerate(muls):
    for j,(i2,v2,a2,b2,w2,_) in enumerate(muls):
        if w!=w2: continue
        if j>k: new+=imp(a,b,f'%uf{k}',a2,b2,f'%uf{j}',w)+imp(a,b,f'%uf{k}',b2,a2,f'%uf{j}',w)
        new+=imp(a,b,f'%uf{k}',f'%ufha{j}',f'%ufhb{j}',f'%ufhr{j}',w)+imp(a,b,f'%uf{k}',f'%ufhb{j}',f'%ufha{j}',f'%ufhr{j}',w)
h=next(i for i,l in enumerate(L) if 'hw.module' in l)
L[h]=L[h].replace('(in ','('+', '.join(ports)+', in ',1)
o=next(i for i,l in enumerate(L) if 'hw.output' in l)
ind=re.match(r'\s*',L[o])[0]
L[o:o]=[ind+x for x in new]
open(sys.argv[2],'w').write('\n'.join(L)); print(len(muls),'multipliers abstracted,',n,'constraints',file=sys.stderr)
