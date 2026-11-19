#!/usr/bin/env bash
# Export every configuration of the briareus evaluation.
set -u
R=/root/yjh-workspace/circt-solver-oriented; O=/root/yjh-workspace/solver-oriented-out
export LD_LIBRARY_PATH=/nix/store/41ym1jm1b7j3rhglk82gwg9jml26z1km-gcc-14.3.0-lib/lib CIRCT_BIN=$R/build/bin P=12
E=$R/experiments/solver-oriented/export_cfgs.sh
CVIN=/var/storage/workspaces/clo91eaf/yjh-workspace/synth-solver-out/core-env
PIN=$O/picorv32/none
CVA=$(ls $O/core-env-final/*.sc.aig | xargs -n1 basename | sed 's/\.sc\.aig$//' | tr '\n' ' ')
PA=$(ls $O/picorv32/models | sed 's/\..*//' | sort -u | grep "^insn_\|^csrw_" | tr '\n' ' ')
CS="comb-context-simplify{depth=32 budget=16384},canonicalize,cse,"
CS2="comb-context-simplify{depth=32 budget=16384 or-form cases=4},canonicalize,cse,"
KB="seq-known-bits{context},canonicalize,cse,"
TRIM="seq-trim-registers,canonicalize,cse,"
AGG="hw-aggregate-to-comb,canonicalize,cse,"
ex() { # design out-name env...
  local d=$1 n=$2; shift 2
  local in a; if [ $d = cv32 ]; then in=$CVIN; a=$CVA; else in=$PIN; a=$PA; fi
  echo "== $d $n $(date)"
  env "$@" IN=$in OUT=$O/bench/$d/$n $E $a > $O/bench/$d.$n.log 2>&1
}
# Full configurations (with unsharing and sc).
ex cv32 ws3r WS=2 ADDER=RIPPLE-CARRY
ex pico base NOUS=0
ex pico ws3r WS=2 ADDER=RIPPLE-CARRY
# Ablations: no sc, no unsharing.
for d in cv32 pico; do
  ex $d r NOUS=1 NOSC=1 ADDER=RIPPLE-CARRY
  ex $d ws3 NOUS=1 NOSC=1 WS=2
  ex $d kb NOUS=1 NOSC=1 ADDER=RIPPLE-CARRY XDCP="$KB$TRIM"
  ex $d cs NOUS=1 NOSC=1 ADDER=RIPPLE-CARRY XDCP="$CS$CS2$AGG"
  ex $d nowc NOUS=1 NOSC=1 ADDER=RIPPLE-CARRY XDCP="$KB$CS$CS2$TRIM$AGG"
done
echo all exports done
