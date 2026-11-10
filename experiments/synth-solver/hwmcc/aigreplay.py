#!/usr/bin/env python3
"""aigreplay.py MODEL.aig WITNESS: simulate an AIGER witness (1 / b0 / latch
init / input lines / .). Prints per frame the bad states and constraints,
and whether the witness is a counterexample (a bad state in a frame up to
which all constraints hold)."""
import sys

def parse(path):
    data = open(path, "rb").read()
    nl = data.index(b"\n")
    h = data[:nl].split()
    assert h[0] == b"aig"
    M, I, L, O, A = map(int, h[1:6])
    B = int(h[6]) if len(h) > 6 else 0
    C = int(h[7]) if len(h) > 7 else 0
    pos = nl + 1
    def line():
        nonlocal pos
        e = data.index(b"\n", pos)
        s = data[pos:e]
        pos = e + 1
        return s
    latches = []
    for i in range(L):
        p = line().split()
        nxt = int(p[0])
        lit = 2 * (I + 1 + i)
        reset = int(p[1]) if len(p) > 1 else 0
        latches.append((nxt, None if reset == lit else reset))
    outs = [int(line()) for _ in range(O)]
    bads = [int(line()) for _ in range(B)]
    cons = [int(line()) for _ in range(C)]
    ands = []
    def delta():
        nonlocal pos
        x = 0; sh = 0
        while True:
            b = data[pos]; pos += 1
            x |= (b & 0x7f) << sh
            if not b & 0x80:
                return x
            sh += 7
    for i in range(A):
        lhs = 2 * (I + L + 1 + i)
        r0 = lhs - delta()
        r1 = r0 - delta()
        ands.append((r0, r1))
    return I, latches, outs, bads, cons, ands

def simulate(model, init, inputs):
    I, latches, outs, bads, cons, ands = model
    state = list(init)
    rows = []
    for ins in inputs:
        v = [0] * (1 + I + len(latches) + len(ands))
        for i, b in enumerate(ins):
            v[1 + i] = b
        for i, b in enumerate(state):
            v[1 + I + i] = b
        val = lambda l: v[l >> 1] ^ (l & 1)
        base = 1 + I + len(latches)
        for i, (a, b) in enumerate(ands):
            v[base + i] = val(a) & val(b)
        rows.append(([val(x) for x in outs + bads], [val(c) for c in cons], list(state)))
        state = [val(n) for n, _ in latches]
    return rows

def read_witness(path):
    lines = [l.strip() for l in open(path) if l.strip()]
    i = 0
    while not (lines[i] in ("0", "1", "2")):
        i += 1
    assert lines[i] == "1", lines[i]
    i += 2
    init = [int(c) if c in "01" else 0 for c in lines[i]]
    i += 1
    inputs = []
    while i < len(lines) and lines[i] != ".":
        inputs.append([int(c) if c in "01" else 0 for c in lines[i]])
        i += 1
    return init, inputs

if __name__ == "__main__":
    model = parse(sys.argv[1])
    init, inputs = read_witness(sys.argv[2])
    for i, (_, reset) in enumerate(model[1]):
        if reset is not None and reset != init[i]:
            print(f"latch {i}: witness init {init[i]} != reset {reset}")
    rows = simulate(model, init, inputs)
    ok = True; cex = None
    for t, (bad, cons, _) in enumerate(rows):
        ok = ok and all(cons)
        if any(bad) and ok and cex is None:
            cex = t
        print(t, "bad", "".join(map(str, bad)), "cons", "".join(map(str, cons)))
    print("counterexample at frame", cex)
