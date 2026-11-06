#!/usr/bin/env python3
"""Write a formal version of bhv/cv32e40x_wrapper.sv: the core with all SVA
bound, without the simulation-only parts that circt-verilog cannot import
(core_log and rvfi read signals by hierarchical names; the rvfi SVA and the
tracer are bound to rvfi).

    make_wrapper.py CV32E40X_DIR OUT.sv [--drop MODULE,...]

--drop removes the SVA bound to cv32e40x_MODULE (for SVA that circt-verilog
cannot import yet).
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
  text = cut(text, "    cv32e40x_core_log\n", r"core_log_i\(.*?\);\n")
  text = cut(text, "    cv32e40x_rvfi\n", r"rvfi_i\s*\(.*?\n\s*\);\n")
  text = cut(text, "`ifndef FORMAL\n  bind cv32e40x_rvfi:", r"`endif\n")
  text = cut(text, "  bind cv32e40x_rvfi:", r"\.\*\);\n")
  assert "rvfi_i" not in text and "core_log_i" not in text
  if "--drop" in sys.argv:
    for m in sys.argv[sys.argv.index("--drop") + 1].split(","):
      while f"bind cv32e40x_{m}:" in text:
        text = cut(text, f"bind cv32e40x_{m}:", r"\);\s*\n")
  open(sys.argv[2], "w").write(text)


if __name__ == "__main__":
  main()
