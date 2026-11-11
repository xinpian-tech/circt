#!/usr/bin/env python3
"""Keep only some invariant constraints of a binary AIGER file.

  aigkeepcons.py IN.aig OUT.aig [I,J,...]

The constraints (AIGER 1.9 C section) are numbered from 0; with an empty
list all are removed. Used to find which constraints remove a
counterexample.
"""
import sys


def main():
  data = open(sys.argv[1], "rb").read()
  lines = data.split(b"\n")
  h = lines[0].split()
  m, i, l, o, a = (int(x) for x in h[1:6])
  b, c, j, f = ([int(x) for x in h[6:]] + [0] * 4)[:4]
  assert b == j == f == 0, "only outputs and constraints"
  keep = [
      int(x) for x in (sys.argv[3] if len(sys.argv) > 3 else "").split(",") if x
  ]
  cons = lines[1 + l + o:1 + l + o + c]
  start = sum(len(x) + 1 for x in lines[:1 + l + o + c])
  with open(sys.argv[2], "wb") as g:
    g.write(b"aig %d %d %d %d %d" % (m, i, l, o, a))
    g.write(b" 0 %d\n" % len(keep) if keep else b"\n")
    for x in lines[1:1 + l + o]:
      g.write(x + b"\n")
    for k in keep:
      g.write(cons[k] + b"\n")
    g.write(data[start:])


if __name__ == "__main__":
  main()
