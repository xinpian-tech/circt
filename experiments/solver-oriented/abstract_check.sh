#!/usr/bin/env bash
# Abstract the multipliers of a model, run BMC, and replay a counterexample
# against real multiplication.
#   CIRCT_BIN=dir OUT=dir K=20 TL=300 abstract_check.sh NAME IN.mlir [PRE-PASSES]
# PRE-PASSES (comma-separated hw.module passes) run before the abstraction,
# e.g. comb-unshare-operators. Prints: NAME result depth seconds, and for a
# counterexample the verdict of replay.py and the first disagreeing cycles.
set -u
H=$(cd "$(dirname "$0")" && pwd)
Y=/var/storage/workspaces/clo91eaf/yjh-workspace
RBMC=${RBMC:-$Y/rbmc-bundle/rbmc}; AIGSIM=${AIGSIM:-$Y/rbmc-bundle/aigsim}
B=${CIRCT_BIN:?}; OUT=${OUT:?}; K=${K:-20}; TL=${TL:-300}
a=$1; in=$2; pre=${3:+$3,}
mkdir -p $OUT
POST="canonicalize,cse,hw-cone-of-influence,canonicalize,hw-aggregate-to-comb,canonicalize,synth-lower-word-to-bits,canonicalize,convert-comb-to-synth,synth-lower-word-to-bits,canonicalize,synth-remove-false-cycles{obligation fixpoint-obligation},synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash"
$B/circt-opt $in --pass-pipeline="builtin.module(hw.module(canonicalize,cse,${pre}canonicalize,cse,verif-abstract-operators{observe},$POST))" \
  -o $OUT/$a.abs.mlir 2>$OUT/$a.abs.log &&
  $B/circt-translate --export-aiger $OUT/$a.abs.mlir -o $OUT/$a.abs.full.aig &&
  python3 $H/../synth-solver/cv32e40x/aigsel.py $OUT/$a.abs.full.aig $OUT/$a.abs.aig 0 || { echo "$a EXPORT-FAILED"; exit 1; }
rm -f $OUT/$a.abs.mlir
s=$(date +%s.%N)
timeout $TL $RBMC $OUT/$a.abs.aig --end $K --witness > $OUT/$a.wit 2>$OUT/$a.rbmc.err
t=$(printf "%.1f" "$(echo "$(date +%s.%N)-$s" | bc)")
if head -1 $OUT/$a.wit | grep -q '^SAT'; then
  tail -n +2 $OUT/$a.wit > $OUT/$a.wit2
  $AIGSIM -w $OUT/$a.abs.full.aig $OUT/$a.wit2 > $OUT/$a.sim 2>/dev/null
  d=$(($(grep -c '^[01x]' $OUT/$a.wit2) - 2))
  python3 $H/replay.py $OUT/$a.abs.full.aig $OUT/$a.sim > $OUT/$a.replay
  echo "$a SAT $d $t $(tail -1 $OUT/$a.replay)"
  grep -v 'lhs=0x0 rhs=0x0' $OUT/$a.replay | head -${SHOW:-6}
elif grep -q 'no counterexample up to bound' $OUT/$a.wit $OUT/$a.rbmc.err 2>/dev/null; then
  echo "$a K $(grep -ho 'up to bound [0-9]*' $OUT/$a.wit $OUT/$a.rbmc.err | tail -1 | grep -o '[0-9]*') $t"
else echo "$a TO ? $t"; fi
