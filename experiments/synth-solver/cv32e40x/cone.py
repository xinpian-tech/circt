#!/usr/bin/env python3
"""Show the combinational cone of `bad` in a model written by lower_regs.py.

    cone.py MODEL.mlir           # size of the cone and number of comb.mul
    cone.py MODEL.mlir --print   # the ops of the cone, from bad down

The walk stops at registers and inputs. Use it to see what a pass left of an
assertion (e.g. whether a multiplier miter is still there).
"""

import re
import sys


def main():
  path = sys.argv[1]
  lines = open(path).read().splitlines()
  defs = {}
  for l in lines:
    m = re.match(r"\s*(%\S+) = (.*)", l)
    if m:
      defs[m.group(1)] = m.group(2)
  out = next(l for l in lines if "hw.output" in l).split()[1]
  seen, stack = [], [out]
  while stack:
    n = stack.pop()
    if n in seen or n not in defs or "seq.compreg" in defs[n]:
      continue
    seen.append(n)
    stack += re.findall(r"%[\w.$-]+", defs[n])
  if "--print" in sys.argv:
    for n in seen:
      print(n, "=", defs[n][:160])
  else:
    muls = sum("comb.mul" in defs[n] for n in seen)
    print(f"cone {len(seen)} muls {muls}")


if __name__ == "__main__":
  main()
