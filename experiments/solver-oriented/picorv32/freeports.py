#!/usr/bin/env python3
"""Make the free values of riscv-formal explicit, for CIRCT, as inputs of the
top module pico_top (CIRCT keeps an undriven declaration as a signal without
a value, and folds a register that only holds itself to a constant):
- `rvformal_rand_reg` of the wrapper (YOSYS: `rand reg`, any value in every
  cycle): an input, passed down through the testbench;
- `rvformal_rand_const_reg` of a checker (YOSYS: `rand const reg`, any value,
  the same in every cycle): an input NAME_free sampled in the first cycle and
  held in a register afterwards.
  freeports.py DIR CHECKER.sv   rewrites DIR/wrapper.sv, DIR/rvfi_testbench.sv,
                                DIR/CHECKER.sv and writes DIR/top.sv
"""
import re, sys

d, checker = sys.argv[1], sys.argv[2]


def decls(text, macro):
    pat = re.compile(r"^\s*(?:\(\*[^*]*\*\)\s*)?`" + macro +
                     r"\s*(\[[^\]]*\])?\s*(\w+)\s*;\s*$", re.M)
    return pat, [(m.group(1) or "", m.group(2)) for m in pat.finditer(text)]


def add_ports(text, module, ports):
    return re.sub(r"(module " + module + r"\s*\()",
                  lambda m: m.group(1) + "\n" + ports, text, count=1)


# Wrapper: any value in every cycle.
w = open(f"{d}/wrapper.sv").read()
pat, seq = decls(w, "rvformal_rand_reg")
w = add_ports(pat.sub("", w), "rvfi_wrapper",
              "".join(f"\tinput {r} {n},\n" for r, n in seq))
open(f"{d}/wrapper.sv", "w").write(w)

# Checker: any value, held.
c = open(f"{d}/{checker}").read()
pat, const = decls(c, "rvformal_rand_const_reg")


def held(m):
    r, n = m.group(1) or "", m.group(2)
    return (f"\treg {r} {n}_q;\n\treg {n}_sampled = 1'b0;\n"
            f"\talways @(posedge clock) begin {n}_sampled <= 1'b1; "
            f"if (!{n}_sampled) {n}_q <= {n}_free; end\n"
            f"\twire {r} {n} = {n}_sampled ? {n}_q : {n}_free;\n")


c = pat.sub(held, c)
cmod = re.search(r"^module (\w+)\s*\(", c, re.M).group(1)
c = add_ports(c, cmod, "".join(f"\tinput {r} {n}_free,\n" for r, n in const))
open(f"{d}/{checker}", "w").write(c)

# Testbench: pass them down.
t = open(f"{d}/rvfi_testbench.sv").read()
free = seq + [(r, n + "_free") for r, n in const]
t = t.replace("\tinput clock, reset\n",
              "".join(f"\tinput {r} {n},\n" for r, n in free) +
              "\tinput clock, reset\n", 1)
t = t.replace("rvfi_wrapper wrapper (\n", "rvfi_wrapper wrapper (\n" +
              "".join(f"\t\t.{n} ({n}),\n" for _, n in seq), 1)
t = t.replace("`RISCV_FORMAL_CHECKER checker_inst (\n",
              "`RISCV_FORMAL_CHECKER checker_inst (\n" +
              "".join(f"\t\t.{n}_free ({n}_free),\n" for _, n in const), 1)
open(f"{d}/rvfi_testbench.sv", "w").write(t)

top = ["module pico_top(input clock" +
       "".join(f", input {r} {n}" for r, n in free) + ");",
       "  reg reset = 1'b1;",
       "  always @(posedge clock) reset <= 1'b0;",
       "  rvfi_testbench tb(.clock(clock), .reset(reset)" +
       "".join(f", .{n}({n})" for _, n in free) + ");",
       "endmodule"]
open(f"{d}/top.sv", "w").write("\n".join(top) + "\n")
print(" ".join(n for _, n in free))
