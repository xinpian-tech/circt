#!/usr/bin/env python3
"""Explicit-state reachability on a small binary AIGER file (for tests).

Latches start at 0 (as written by circt-translate --export-aiger). Prints
SAT and the depth if an output (or bad) can be 1 in a reachable state, else
UNSAT. Enumerates all input values per step: only for tiny models.

    aigreach.py MODEL.aig [--max-inputs 16]
"""

import itertools
import sys


def read_aig(path):
    data = open(path, "rb").read()
    nl = data.index(b"\n")
    hdr = data[:nl].split()
    assert hdr[0] == b"aig", "binary AIGER only"
    M, I, L, O, A = map(int, hdr[1:6])
    B = int(hdr[6]) if len(hdr) > 6 else 0
    pos = nl + 1
    lines = []
    for _ in range(L + O + B):
        e = data.index(b"\n", pos)
        lines.append(data[pos:e].split())
        pos = e + 1
    latches = []
    for k in range(L):
        f = lines[k]
        init = int(f[1]) if len(f) > 1 else 0
        latches.append((int(f[0]), init))
    outs = [int(lines[L + k][0]) for k in range(O + B)]

    def num():
        nonlocal pos
        x, s = 0, 0
        while True:
            b = data[pos]
            pos += 1
            x |= (b & 0x7F) << s
            s += 7
            if not b & 0x80:
                return x
    ands = []
    for k in range(A):
        lhs = 2 * (I + L + k + 1)
        d0 = num()
        d1 = num()
        r0 = lhs - d0
        ands.append((lhs, r0, r0 - d1))
    return I, L, latches, outs, ands


def main():
    path = sys.argv[1]
    max_in = int(sys.argv[3]) if len(sys.argv) > 3 else 16
    I, L, latches, outs, ands = read_aig(path)
    if I > max_in:
        raise SystemExit(f"{I} inputs, too many")
    assert all(init in (0, 1) for _, init in latches), "X init not handled"

    def ev(state, inp):
        v = {0: 0}
        for k in range(I):
            v[2 * (k + 1)] = inp[k]
        for k in range(L):
            v[2 * (I + k + 1)] = state[k]
        lit = lambda l: v[l & ~1] ^ (l & 1)
        for lhs, a, b in ands:
            v[lhs] = lit(a) & lit(b)
        return [lit(n) for n, _ in latches], [lit(o) for o in outs]

    frontier = {tuple(init for _, init in latches)}
    seen = set(frontier)
    depth = 0
    while frontier:
        nxt = set()
        for s in frontier:
            for inp in itertools.product((0, 1), repeat=I):
                ns, o = ev(s, inp)
                if any(o):
                    print(f"SAT depth {depth}")
                    return
                ns = tuple(ns)
                if ns not in seen:
                    seen.add(ns)
                    nxt.add(ns)
        frontier = nxt
        depth += 1
    print(f"UNSAT ({len(seen)} reachable states)")


if __name__ == "__main__":
    main()
