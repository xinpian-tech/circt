#!/usr/bin/env python3
"""shiftwit.py NEW.mlir ORIG.aig NEWWITNESS K > ORIGWITNESS: map a witness of
the temporally decomposed module (prefix K) to one of the original."""
import sys, re
sys.path.insert(0, __import__("os").path.dirname(__file__))
from aigreplay import parse, read_witness
mlir, orig, wit, K = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
header = next(l for l in open(mlir) if "hw.module" in l)
ports = [p for p in re.findall(r'in %(\S+?)(?: "[^"]*")? : (\S+?)[,)]', header) if p[1] == "i1"]
names = [p[0] for p in ports]
I, latches, *_ = parse(orig)
init, inputs = read_witness(wit)
first = dict(zip(names, inputs[0]))
# latch init: reset or initN_pre0
oinit = []
for j, (_, reset) in enumerate(latches):
    oinit.append(reset if reset is not None else first.get(f"init{j}_pre0", 0))
rows = []
for t in range(K):
    rows.append([first.get(f"{names[i]}_pre{t}", 0) for i in range(I)])
for ins in inputs:
    rows.append(ins[:I])
print("1\nb0")
print("".join(map(str, oinit)))
for r in rows:
    print("".join(map(str, r)))
print(".")
