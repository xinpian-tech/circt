#!/usr/bin/env python3
"""Compare front-end configurations from run_rbmc.sh output.

usage: summarize_rbmc.py OUT.tsv [K TL]

Per configuration: counterexamples found, assertions checked up to bound K,
timeouts (with the mean bound reached), a PAR-2 score (time to a
counterexample or to bound K; 2 TL otherwise), the time on the assertions
that every configuration finishes, and the geometric mean of the time ratio
to `base` on those. Soundness checks across configurations: a counterexample
must have the same length in every configuration that finds one, no
configuration may check bound K without counterexample where another has a
counterexample of length <= K, and every witness must pass aigsim -c.
"""
import collections
import math
import sys

rows = [l.rstrip("\n").split("\t") for l in open(sys.argv[1]) if l.strip()]
K = int(sys.argv[2]) if len(sys.argv) > 2 else 20
TL = float(sys.argv[3]) if len(sys.argv) > 3 else 300
r = collections.defaultdict(dict)
for a, c, res, dep, t, w in rows:
    r[c][a] = (res, int(dep) if dep else None, float(t) if t else None, w)
cfgs = list(dict.fromkeys(c for _, c, *_ in rows))
asserts = sorted({a for a, *_ in rows})
done = lambda x: x[0] in ("SAT", "K") and x[2] <= TL
common = [a for a in asserts if all(a in r[c] and done(r[c][a]) for c in cfgs)]
base = "base" if "base" in cfgs else cfgs[0]

print(f"{len(asserts)} assertions, K = {K}, {TL:.0f} s; {len(common)} finished by every configuration")
print(f"{'config':12} {'SAT':>4} {'K':>4} {'TO':>4} {'reached':>8} {'PAR-2':>7} {'common s':>9} {'ratio':>6}")
for c in cfgs:
    x = r[c]
    n = collections.Counter(x[a][0] for a in asserts if a in x)
    tos = [x[a][1] or 0 for a in asserts if a in x and x[a][0] == "TO"]
    par2 = sum(x[a][2] if a in x and done(x[a]) else 2 * TL for a in asserts) / len(asserts)
    ct = sum(x[a][2] for a in common)
    gm = math.exp(sum(math.log(max(x[a][2], 0.05) / max(r[base][a][2], 0.05)) for a in common)
                  / max(len(common), 1))
    reached = f"{sum(tos) / len(tos):.1f}" if tos else "-"
    print(f"{c:12} {n['SAT']:4} {n['K']:4} {n['TO']:4} {reached:>8} {par2:7.1f} {ct:9.1f} {gm:6.2f}")

bad = []
for a in asserts:
    xs = {c: r[c][a] for c in cfgs if a in r[c]}
    sat = {c: x[1] for c, x in xs.items() if x[0] == "SAT"}
    if len(set(sat.values())) > 1:
        bad.append(f"{a}: counterexample lengths differ {sat}")
    if sat:
        d = min(sat.values())
        if d <= K:
            for c, x in xs.items():
                if x[0] == "K":
                    bad.append(f"{a}: {c} checks bound {K} without counterexample, but has one of length {d}")
    for c, x in xs.items():
        if x[3] == "BAD":
            bad.append(f"{a}: {c} witness fails aigsim -c")
        if x[0] == "MISSING":
            bad.append(f"{a}: {c} model missing")
print(f"\nsoundness problems: {len(bad)}")
for b in bad:
    print("  " + b)
