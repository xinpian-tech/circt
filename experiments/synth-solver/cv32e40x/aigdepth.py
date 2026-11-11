#!/usr/bin/env python3
"""Vacuity check: replace the output of a binary AIGER file by a chain of N
fresh latches (init 0, first fed by 1), so the output is 1 exactly from
cycle N on. Constraints are kept. A BMC counterexample of length N then
shows that the constraints can hold in cycles 0..N.

  aigdepth.py IN.aig OUT.aig N
"""
import sys


def varints(data, pos, n):
  out = []
  for _ in range(n):
    x, s = 0, 0
    while True:
      b = data[pos]
      pos += 1
      x |= (b & 0x7f) << s
      s += 7
      if not b & 0x80:
        break
    out.append(x)
  return out, pos


def enc(x):
  b = bytearray()
  while x >= 0x80:
    b.append((x & 0x7f) | 0x80)
    x >>= 7
  b.append(x)
  return bytes(b)


inp, outp, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
data = open(inp, "rb").read()
lines = data.split(b"\n")
h = lines[0].split()
m, i, l, o, a = (int(x) for x in h[1:6])
b, c, j, f = ([int(x) for x in h[6:]] + [0] * 4)[:4]
assert b == j == f == 0
latches = [list(map(int, x.split())) for x in lines[1:1 + l]]
cons = [int(x) for x in lines[1 + l + o:1 + l + o + c]]
start = sum(len(x) + 1 for x in lines[:1 + l + o + c])
d, _ = varints(data, start, 2 * a)
first = i + l  # last old latch var


def mp(lit):
  v = lit >> 1
  return lit + 2 * n if v > first else lit


ands = []
for k in range(a):
  lhs = 2 * (first + 1 + k)
  r0 = lhs - d[2 * k]
  r1 = r0 - d[2 * k + 1]
  ands.append((mp(r0), mp(r1)))
new = [2 * (first + 1 + t) for t in range(n)]
with open(outp, "wb") as g:
  hdr = b"aig %d %d %d 1 %d" % (m + n, i, l + n, a)
  g.write(hdr + (b" 0 %d\n" % c if c else b"\n"))
  for x in latches:
    g.write(b" ".join(b"%d" % mp(y) if k == 0 else b"%d" % y
                      for k, y in enumerate(x)) + b"\n")
  for t in range(n):
    g.write(b"%d 0\n" % (1 if t == 0 else new[t - 1]))
  g.write(b"%d\n" % new[-1])
  for x in cons:
    g.write(b"%d\n" % mp(x))
  for k, (r0, r1) in enumerate(ands):
    lhs = 2 * (first + n + 1 + k)
    g.write(enc(lhs - r0) + enc(r0 - r1))
