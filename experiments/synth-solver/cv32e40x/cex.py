#!/usr/bin/env python3
"""Print an rIC3 counterexample with the input names of the AIG.

    cex.py MODEL.aig CEX.txt [--all]

MODEL.aig must have a symbol table (circt-translate --export-aiger without
--exclude-symbol-table); CEX.txt is the output of `ric3 check --cex`.
Input bits are regrouped into words (name[k]); words that are 0 are left
out unless --all.
"""

import collections
import re
import sys


def symbols(path):
    data = open(path, "rb").read()
    nl = data.index(b"\n")
    hdr = data[:nl].split()
    M, I, L, O, A = map(int, hdr[1:6])
    pos = nl + 1
    nlines = L + O + (int(hdr[6]) if len(hdr) > 6 else 0)
    for _ in range(nlines):
        pos = data.index(b"\n", pos) + 1
    for _ in range(A):  # two varints per AND
        for _ in range(2):
            while data[pos] & 0x80:
                pos += 1
            pos += 1
    sym = {}
    for line in data[pos:].split(b"\n"):
        if line == b"c":
            break
        m = re.match(rb"i(\d+) (.*)", line)
        if m:
            sym[int(m.group(1))] = m.group(2).decode(errors="replace")
    return sym


def main():
    aig, cex = sys.argv[1], sys.argv[2]
    show_all = "--all" in sys.argv
    sym = symbols(aig)
    lines = open(cex).read().splitlines()
    k = lines.index("SAT")
    # SAT, property index, b<k>, initial latches, one line per step, "."
    steps = []
    for l in lines[k + 4:]:
        if l == ".":
            break
        steps.append(l)
    prev = None
    for t, st in enumerate(steps):
        words = collections.OrderedDict()
        for j, b in enumerate(st):
            name = sym.get(j, f"i{j}")
            m = re.match(r"(.*)\[(\d+)\]$", name)
            base, bit = (m.group(1), int(m.group(2))) if m else (name, 0)
            words[base] = words.get(base, 0) | (int(b) << bit) \
                if b in "01" else words.get(base, 0)
        shown = {n: v for n, v in words.items() if v or show_all}
        print(f"-- step {t}")
        for n, v in shown.items():
            mark = "" if prev is None or prev.get(n) == v else "  *"
            print(f"   {n} = {v:#x}{mark}" if v > 1 else f"   {n} = {v}{mark}")
        prev = words


if __name__ == "__main__":
    main()
