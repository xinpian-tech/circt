#!/usr/bin/env python3
"""Check a counterexample of a model abstracted by
verif-abstract-operators{observe} against real multiplication.
  replay.py FULL.aig TRACE
FULL.aig: the exported model with all outputs and the symbol table;
TRACE: output of `aigsim -w FULL.aig WITNESS` (state inputs outputs next).
Prints, per cycle, the multipliers whose free result differs from the
product of the operands. Without such a cycle the counterexample is one of
the original model."""
import re,sys,subprocess,collections
syms=collections.defaultdict(dict)
data=open(sys.argv[1],'rb').read()
for m in re.finditer(rb'\no(\d+) abstract_mul_(\d+)_obs_(lhs|rhs|result)\[(\d+)\]',data):
    syms[(int(m[2]),m[3].decode())][int(m[4])]=int(m[1])
muls=sorted({k[0] for k in syms})
bad=0
for t,line in enumerate(open(sys.argv[2])):
    f=line.split()
    if len(f)!=4: continue
    out=f[2]
    def word(k,part):
        bits=syms[(k,part)]; return sum((out[bits[i]]=='1')<<i for i in bits), len(bits)
    for k in muls:
        (a,w),(b,_),(r,_)=word(k,'lhs'),word(k,'rhs'),word(k,'result')
        true=(a*b)&((1<<w)-1)
        if true!=r:
            bad+=1; d=true^r
            print(f'cycle {t} mul {k} (i{w}): lhs={a:#x} rhs={b:#x} free={r:#x} product={true:#x} differing bits {d.bit_length()-1}..{(d&-d).bit_length()-1}')
print('SPURIOUS' if bad else 'REAL: every free result equals the product')
