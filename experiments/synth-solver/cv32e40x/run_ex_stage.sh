#!/usr/bin/env bash
# ex_stage baseline: one model per SVA assertion (mult, div) under the
# environment assumption sv/cv32e40x_ex_stage_env_sva.sv, lowered with
# lower.sh + circt-synth, checked with rIC3 IC3.
# Output TSV on stdout: assert latches ands result seconds
set -u
H=$(cd "$(dirname "$0")" && pwd)
source $H/../env.sh
RIC3=${RIC3:-/var/storage/workspaces/clo91eaf/yjh-workspace/rIC3-target-rebuild-20261106/release/ric3}
OUT=${OUT:-$(mktemp -d)}; TL=${TL:-120}; P=${P:-12}
mkdir -p $OUT
$H/import.sh cv32e40x_ex_stage $OUT/ex.mlir ex_stage_env mult div
export H OUT RIC3 TL CIRCT_PY
one() { l=$1; f=$OUT/$l
  $H/lower.sh $OUT/ex.mlir $f.mlir --assert $l --pre-cache $OUT/pre >$f.log 2>&1 || { printf "%s\t\t\tPREP_FAIL\t\n" $l; return; }
  circt-synth $f.mlir 2>>$f.log | circt-translate --export-aiger -o $f.aig 2>>$f.log || { printf "%s\t\t\tSYNTH_FAIL\t\n" $l; return; }
  s=$(date +%s.%N)
  timeout $TL $RIC3 check $f.aig --ui false --cex ic3 --rseed 0 >$f.cex 2>/dev/null
  r=$(grep -aoE '^(SAT|UNSAT)$' $f.cex | tail -1)
  printf "%s\t%s\t%s\t%s\t%.1f\n" $l "$(head -1 $f.aig | awk '{print $4"\t"$6}')" "${r:-TO}" "$(echo "$(date +%s.%N)-$s" | bc)"; }
export -f one
printf "assert\tlatches\tands\tresult\tseconds\n"
$H/lower.sh $OUT/ex.mlir --list --pre-cache $OUT/pre 2>/dev/null |
  xargs -P $P -I{} bash -c 'one {}' | sort
