#!/usr/bin/env python3
"""Rewrite a binary AIGER 1.9 model as a plain one for tools that read
latches as starting at 0 and do not know constraints (ABC).

  aigplain.py IN.aig OUT.aig

IN has one output (the bad state), latches with initial value 0, 1 or
unknown, and invariant constraints. OUT has one output, latches that all
start at 0 and no constraints, with the same counterexample lengths:
- a latch that starts at 1 is stored complemented;
- a latch with an unknown initial value reads a new input in the first
  cycle (a new latch `nf` is 0 in the first cycle and 1 afterwards);
- the output is the bad state and all constraints, while no constraint was
  violated before (a new latch remembers a violation).
"""
import sys


def read(path):
  data = open(path, "rb").read()
  lines = data.split(b"\n")
  h = lines[0].split()
  assert h[0] == b"aig", "binary AIGER expected"
  m, i, l, o, a = (int(x) for x in h[1:6])
  b, c, j, f = ([int(x) for x in h[6:]] + [0] * 4)[:4]
  assert j == f == 0 and o + b == 1, "one output or bad state expected"
  latches = []
  for k in range(l):
    t = [int(x) for x in lines[1 + k].split()]
    init = t[1] if len(t) > 1 else 0
    latches.append((t[0], init if init in (0, 1) else None))
  rest = [int(x) for x in lines[1 + l:1 + l + o + b + c]]
  bad, cons = rest[0], rest[1:]
  pos = sum(len(x) + 1 for x in lines[:1 + l + o + b + c])

  def varint():
    nonlocal pos
    x = s = 0
    while True:
      ch = data[pos]
      pos += 1
      x |= (ch & 0x7f) << s
      if not ch & 0x80:
        return x
      s += 7

  ands = []
  for k in range(a):
    lhs = 2 * (i + l + k + 1)
    r0 = lhs - varint()
    ands.append((r0, r0 - varint()))
  return i, latches, bad, cons, ands


def main():
  i, latches, bad, cons, ands = read(sys.argv[1])
  l = len(latches)
  xs = [k for k, (_, init) in enumerate(latches) if init is None]
  ni = i + len(xs)
  nl = l + (1 if xs else 0) + (1 if cons else 0)
  out = []  # new ands (r0, r1); the and k has variable ni + nl + 1 + k

  def mk(x, y):
    if x == 0 or y == 0 or x == y ^ 1:
      return 0
    if x == 1 or x == y:
      return y
    if y == 1:
      return x
    out.append((max(x, y), min(x, y)))
    return 2 * (ni + nl + len(out))

  lat = lambda k: 2 * (ni + 1 + k)
  nf = lat(l) if xs else 1
  viol = lat(nl - 1) if cons else 0
  mp = [0] * (i + l + len(ands) + 1)  # old variable -> new literal
  for k in range(i):
    mp[k + 1] = 2 * (k + 1)
  xin = {k: 2 * (i + 1 + n) for n, k in enumerate(xs)}
  for k, (_, init) in enumerate(latches):
    if init is None:  # nf ? latch : input
      mp[i + 1 + k] = mk(mk(nf, lat(k)) ^ 1, mk(nf ^ 1, xin[k]) ^ 1) ^ 1
    else:
      mp[i + 1 + k] = lat(k) ^ init
  lit = lambda x: mp[x >> 1] ^ (x & 1)
  for k, (r0, r1) in enumerate(ands):
    mp[i + l + 1 + k] = mk(lit(r0), lit(r1))
  nexts = [lit(n) ^ (init or 0) for n, init in latches]
  if xs:
    nexts.append(1)
  o = lit(bad)
  if cons:
    call = 1
    for c in cons:
      call = mk(call, lit(c))
    o = mk(mk(o, call), viol ^ 1)
    nexts.append(mk(viol ^ 1, call) ^ 1)

  def varint(x):
    r = bytearray()
    while x >= 0x80:
      r.append(x & 0x7f | 0x80)
      x >>= 7
    r.append(x)
    return bytes(r)

  with open(sys.argv[2], "wb") as g:
    g.write(b"aig %d %d %d 1 %d\n" % (ni + nl + len(out), ni, nl, len(out)))
    for n in nexts:
      g.write(b"%d\n" % n)
    g.write(b"%d\n" % o)
    for k, (r0, r1) in enumerate(out):
      lhs = 2 * (ni + nl + 1 + k)
      g.write(varint(lhs - r0) + varint(r0 - r1))


if __name__ == "__main__":
  main()
