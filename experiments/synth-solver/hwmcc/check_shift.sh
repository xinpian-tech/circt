#!/usr/bin/env bash
# Check synth-temporal-decomposition on an AIGER model with rBMC:
#   OPTS="force" END=20 TL=40 OUT=dir check_shift.sh MODEL.aig
# The model is imported, decomposed (bad-outputs, plus OPTS) and exported.
# With a prefix k > 0 (or ALL=1), rBMC runs on both models: counterexample
# depths must differ by k (0 if the original fails within the prefix), no
# counterexample may appear or disappear within the bounds reached, and the
# witness of the new model, mapped back (shiftwit.py), must be a
# counterexample of the original when replayed (aigreplay.py). ABC is not
# used: it starts latches without initial value at 0.
# Output: name, I L A before, after, k/transient/constant, results, verdict.
H=$(cd "$(dirname "$0")" && pwd)
source $H/../env.sh
RBMC=${RBMC:-/var/storage/workspaces/clo91eaf/yjh-workspace/rbmc-bundle/rbmc}
f=$1; n=$(basename $f .aig); OUT=${OUT:-$(mktemp -d)}; mkdir -p $OUT/$n; cd $OUT/$n
END=${END:-20}; TL=${TL:-40}
timeout 300 bash -c "circt-translate --import-aiger $f | circt-opt --pass-pipeline='builtin.module(hw.module(synth-temporal-decomposition{bad-outputs ${OPTS:-}},synth-lower-variadic))' --mlir-pass-statistics 2>log -o new.mlir && circt-translate --export-aiger --exclude-symbol-table new.mlir -o new.aig 2>>log" || { echo -e "$n\tFAIL"; exit; }
st() { grep -oE "[0-9]+ $1 " log | grep -oE '^[0-9]+'; }
k=$(st prefix); k=${k:-0}
res="$n\t$(head -1 $f | cut -d' ' -f3,4,6)\t$(head -1 new.aig | cut -d' ' -f3,4,6)\tk=$k tr=$(st num-transient) c=$(st num-constant)"
bmc() {
  timeout $((TL+20)) $RBMC $1 --end $2 --time-limit $TL --witness > $3 2>$3.err
  if grep -q '^SAT' $3; then
    echo "cex $(cat $3 $3.err | grep -oE 'length [0-9]+' | grep -oE '[0-9]+')"
  else
    echo "none $(cat $3 $3.err | grep -oE 'bound [0-9]+' | grep -oE '[0-9]+' | tail -1)"
  fi
}
if [ $k -gt 0 ] || [ -n "${ALL:-}" ]; then
  r0=$(bmc $f $END orig.out); r1=$(bmc new.aig $((END-k)) new.out)
  verdict=OK
  set -- $r0; t0=$1; d0=${2:--1}; set -- $r1; t1=$1; d1=${2:--1}
  if [ $t1 = cex ]; then
    # Without a prefix the latches keep their own initial values, which
    # shiftwit.py cannot map (the latch order changes): depths only.
    if [ $k -gt 0 ]; then
      sed -n '/^1$/,$p' new.out > new.wit
      python3 $H/shiftwit.py new.mlir $f new.wit $k > orig.wit
      m=$(python3 $H/aigreplay.py $f orig.wit | tail -1 | grep -oE '[0-9]+$')
      [ -n "$m" ] && [ $m -le $((d1+k)) ] || verdict="BAD-WITNESS(${m:-none})"
    fi
    if [ $t0 = cex ]; then
      [ $d1 = $((d0>k ? d0-k : 0)) ] || verdict="$verdict DEPTH"
    elif [ $d0 -ge $((d1+k)) ]; then
      verdict="$verdict SPURIOUS"
    fi
  elif [ $t0 = cex ] && [ $d1 -ge $((d0>k ? d0-k : 0)) ]; then
    verdict=MISSED
  fi
  res="$res\torig $r0\tnew $r1\t$verdict"
fi
echo -e "$res"
