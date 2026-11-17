#!/usr/bin/env python3
"""Make the free values of a riscv-formal wrapper (`rvformal_rand_reg`, which
YOSYS reads as `rand reg`, any value in every cycle) inputs of the top module:
CIRCT keeps an undriven declaration as a signal without a value.
  freeports.py DIR   rewrites DIR/wrapper.sv, DIR/rvfi_testbench.sv, DIR/top.sv
"""
import re, sys

d = sys.argv[1]
w = open(f"{d}/wrapper.sv").read()
decl = re.compile(r"^\s*(?:\(\*[^*]*\*\)\s*)?`rvformal_rand_reg\s*(\[[^\]]*\])?\s*(\w+)\s*;\s*$", re.M)
free = [(m.group(1) or "", m.group(2)) for m in decl.finditer(w)]
w = decl.sub("", w)
ports = "".join(f"\tinput {r} {n},\n" for r, n in free)
w = re.sub(r"(module rvfi_wrapper\s*\()", r"\1\n" + ports.replace("\\", "\\\\"), w, count=1)
open(f"{d}/wrapper.sv", "w").write(w)

t = open(f"{d}/rvfi_testbench.sv").read()
t = t.replace("\tinput clock, reset\n", "".join(f"\tinput {r} {n},\n" for r, n in free) + "\tinput clock, reset\n", 1)
t = t.replace("rvfi_wrapper wrapper (\n", "rvfi_wrapper wrapper (\n" + "".join(f"\t\t.{n} ({n}),\n" for _, n in free), 1)
open(f"{d}/rvfi_testbench.sv", "w").write(t)

top = ["module pico_top(input clock" + "".join(f", input {r} {n}" for r, n in free) + ");",
       "  reg reset = 1'b1;",
       "  always @(posedge clock) reset <= 1'b0;",
       "  rvfi_testbench tb(.clock(clock), .reset(reset)" + "".join(f", .{n}({n})" for _, n in free) + ");",
       "endmodule"]
open(f"{d}/top.sv", "w").write("\n".join(top) + "\n")
print(" ".join(n for _, n in free))
