#!/usr/bin/env python3
"""Write a copy of bhv/cv32e40x_wrapper.sv (the core with all SVA bound),
optionally without the SVA bound to some modules.

    make_wrapper.py CV32E40X_DIR OUT.sv [--drop MODULE,...]

--drop removes the SVA bound to cv32e40x_MODULE (for SVA that circt-verilog
cannot import yet). core_log and rvfi are kept: circt-verilog imports them
(their hierarchical references become cross-module references).
"""

import re
import sys


def cut(text, start, end_re):
  """Remove from the line starting with `start` to the first `end_re`."""
  a = text.index(start)
  m = re.compile(end_re, re.S).search(text, a)
  return text[:a] + text[m.end():]


def main():
  text = open(sys.argv[1] + "/bhv/cv32e40x_wrapper.sv").read()
  if "--drop" in sys.argv:
    for m in sys.argv[sys.argv.index("--drop") + 1].split(","):
      while f"bind cv32e40x_{m}:" in text:
        text = cut(text, f"bind cv32e40x_{m}:", r"\);\s*\n")
  open(sys.argv[2], "w").write(text)


if __name__ == "__main__":
  main()
