#!/usr/bin/env bash
# Import one riscv-formal check of PicoRV32 with circt-verilog.
#   RF=riscv-formal-dir PICO=picorv32.v [BUG=00N] import.sh CHECK OUT.mlir
# CHECK is a name from cores/picorv32/checks (genchecks.py), e.g. insn_add_ch0.
# Without YOSYS the free values of riscv-formal are plain declarations:
# `rvformal_rand_const_reg` a register holding itself with no initial value
# (any value, fixed); `rvformal_rand_reg` of the wrapper becomes an input of
# the top (freeports.py). The reset of the first cycle comes from a register
# (the testbench assumes reset == $initstate under YOSYS only).
set -eu
H=$(cd "$(dirname "$0")" && pwd)
source $H/../../synth-solver/env.sh
RF=${RF:-/root/yjh-workspace/ext/riscv-formal}
PICO=${PICO:-/root/yjh-workspace/ext/picorv32/picorv32.v}
check=$1 out=$(realpath -m "$2")
sby=$RF/cores/picorv32/checks/$check.sby
[ -f $sby ] || { echo "no $sby" >&2; exit 1; }
w=$(mktemp -d)
# Files of the check, with the anyconst declarations made explicit.
for f in $(sed -n '/^\[files\]/,/^\[/p' $sby | grep -v '^\['); do
  sed -E 's/^(\s*)`rvformal_rand_const_reg(\s*(\[[^]]*\])?\s*)([A-Za-z_0-9]+);/\1reg\2\4; always @(posedge clock) \4 <= \4;/' $f > $w/$(basename $f)
done
# Inline files of the sby ([file NAME] sections).
awk -v d=$w '/^\[file /{f=d"/"substr($2,1,length($2)-1); next} /^\[/{f=""} f{print > f}' $sby
# BUG=00N: one of the core's planted bugs (PICORV32_TESTBUG_00N), to check
# that the models can fail.
[ -n "${BUG:-}" ] && sed -i "s/PICORV32_TESTBUG_NONE/PICORV32_TESTBUG_$BUG/" $w/defines.sv
# One compilation unit: the wrapper and the core use the macros of the check.
# The core's `assert of non-formal builds calls an empty task, which CIRCT
# keeps as a coroutine call: use an empty block instead.
cp $RF/cores/picorv32/wrapper.sv $w/
# The free values of the wrapper become inputs of the top module pico_top.
python3 $H/freeports.py $w > /dev/null
sed 's/define assert(assert_expr) empty_statement/define assert(assert_expr) begin end/' $PICO > $w/$(basename $PICO)
{ echo '`timescale 1 ns / 1 ps'
  for f in $check.sv wrapper.sv $(basename $PICO) top.sv; do echo "\`include \"$f\""; done
} > $w/all.sv
cd / && circt-verilog -I $w --mlir-print-debuginfo $w/all.sv --top=pico_top -o $out
rm -rf $w
