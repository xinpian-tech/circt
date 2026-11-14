#!/usr/bin/env python3
"""Which register bits a correspondence pass merged, and which of them are
constants of ternary simulation.

  regmerge.py BASE.mlir RC.mlir TC.mlir

BASE is the model, RC and TC the outputs of synth-register-correspondence
and synth-ternary-constants on it (without canonicalize, so that the
remaining bits of a split register keep the name NAME[i]). Registers
without a name are only counted.
"""
import re,collections,sys
reg=re.compile(r'^\s*(%\S+) = seq\.compreg (?:name "([^"]*)" )?(%\S+), .*: i(\d+)')
def load(p):
    named={}; un=0
    for ln in open(p):
        m=reg.match(ln)
        if not m: continue
        if m.group(2): named[m.group(2)]=int(m.group(4))
        else: un+=int(m.group(4))
    return named,un
def gone(basep,outp):
    base,ub=load(basep); o,uo=load(outp)
    g=set()
    for n,w in base.items():
        if n in o: continue
        for i in range(w):
            if f"{n}[{i}]" not in o: g.add((n,i))
    return base,g,ub-uo
base,grc,urc=gone(sys.argv[1],sys.argv[2]); _,gtc,utc=gone(sys.argv[1],sys.argv[3])
print("named bits",sum(base.values()),"merged by rc",len(grc),"constant by tc",len(gtc),"tc not in rc",len(gtc-grc))
print("unnamed bits merged: rc",urc,"tc",utc)
extra=grc-gtc
print("rc beyond tc (named):",len(extra))
by=collections.defaultdict(list)
for n,i in sorted(extra): by[n].append(i)
for n,b in sorted(by.items(),key=lambda x:-len(x[1])): print(f"  {len(b):3d}/{base[n]:3d} {n} bits {b[:8]}{'...' if len(b)>8 else ''}")
