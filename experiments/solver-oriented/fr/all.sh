#!/usr/bin/env bash
# all.sh : models x configurations: generate, measure (m.sh), cec (cec.sh)
S=$(cd $(dirname $0) && pwd); cd $S
C=/var/storage/workspaces/clo91eaf/yjh-workspace/synth-solver-out/core-env
P=/root/yjh-workspace/solver-oriented-out/picorv32/none-u
one() { # tag input ws adder
  local t=$1 in=$2 ws=$3 ad=$4
  [ -s $t.mlir ] || ./gen.sh $t.mlir $in $ws $ad > /dev/null || return
  [ -s $t.res ] || ./m.sh $t $t.mlir "$FROPT" > /dev/null
  [ -s $t.cec.res ] || ./cec.sh $t $t.mlir "$FROPT" > $t.cec.res
  echo "$(cat $t.res) | $(cat $t.cec.res)"
}
export -f one; export FROPT=${FROPT:-conflict-limit=1000}
{
for c in a_addr_incr a_wb_ready_exception a_mul_result; do
  echo "cv.$c.base $C/$c.none.mlir 0 ''"
  echo "cv.$c.ws $C/$c.none.mlir 2 ''"
  echo "cv.$c.wsr $C/$c.none.mlir 2 RIPPLE-CARRY"
done
for c in insn_add insn_sw csrw_mcycle; do
  echo "pi.$c.base $P/$c.none.mlir 0 ''"
  echo "pi.$c.ws $P/$c.none.mlir 2 ''"
  echo "pi.$c.wsr $P/$c.none.mlir 2 RIPPLE-CARRY"
done
} | xargs -P ${PAR:-2} -L1 bash -c 'one "$@"' _
