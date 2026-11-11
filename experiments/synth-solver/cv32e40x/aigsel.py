#!/usr/bin/env python3
"""Keep one output of a binary AIGER file (outputs are the bad properties).

  aigsel.py IN.aig OUT.aig INDEX

Used to split the model and the false_cycle_obligation output of
synth-remove-false-cycles{obligation} into two models. Inputs, latches,
ANDs and invariant constraints (AIGER 1.9 C section) are kept as they are
(the model checker removes what the output does not use).
"""
import sys


def main():
  inp, out, index = sys.argv[1], sys.argv[2], int(sys.argv[3])
  data = open(inp, "rb").read()
  lines = data.split(b"\n")
  header = lines[0].split()
  assert header[0] == b"aig", "binary AIGER expected"
  m, i, l, o, a = (int(x) for x in header[1:6])
  b, c, j, f = ([int(x) for x in header[6:]] + [0] * 4)[:4]
  assert b == 0 and j == 0 and f == 0, "only outputs and constraints"
  assert 0 <= index < o
  latches = lines[1:1 + l]
  outputs = lines[1 + l:1 + l + o]
  constraints = lines[1 + l + o:1 + l + o + c]
  # The binary ANDs are copied byte for byte: two varints per AND.
  start = sum(len(x) + 1 for x in lines[:1 + l + o + c])
  pos = start
  for _ in range(2 * a):
    while data[pos] & 0x80:
      pos += 1
    pos += 1
  # Symbols: keep those of the inputs and latches and of the kept output.
  symbols, comment = data[pos:], b""
  if b"\nc\n" in b"\n" + symbols:
    k = (b"\n" + symbols).index(b"\nc\n")
    symbols, comment = symbols[:k], symbols[k:]
  kept = []
  for line in symbols.split(b"\n"):
    if not line:
      continue
    if line.startswith(b"o"):
      pos_name = line[1:].split(b" ", 1)
      if int(pos_name[0]) != index:
        continue
      line = b"o0 " + pos_name[1]
    kept.append(line + b"\n")
  with open(out, "wb") as f:
    if c:
      f.write(b"aig %d %d %d 1 %d 0 %d\n" % (m, i, l, a, c))
    else:
      f.write(b"aig %d %d %d 1 %d\n" % (m, i, l, a))
    for x in latches:
      f.write(x + b"\n")
    f.write(outputs[index] + b"\n")
    for x in constraints:
      f.write(x + b"\n")
    f.write(data[start:pos])
    f.write(b"".join(kept))
    f.write(comment)


if __name__ == "__main__":
  main()
