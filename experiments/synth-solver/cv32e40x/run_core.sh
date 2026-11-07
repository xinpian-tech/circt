#!/usr/bin/env bash
# Whole CV32E40X core: one model per SVA assertion, lowered without and with
# verif-simplify-odc (antecedent-only and full), exported to AIGER and checked
# with rIC3 IC3. Identical AIGs (the pass changed nothing) are solved once.
#   OUT=dir TL=60 P=14 MODES="none antecedent full" run_core.sh
# Output TSV on stdout: assert mode latches ands obligation result seconds
set -u
H=$(cd "$(dirname "$0")" && pwd)
source $H/../env.sh
RIC3=${RIC3:-/var/storage/workspaces/clo91eaf/yjh-workspace/rIC3-target-rebuild-20261106/release/ric3}
OUT=${OUT:-$(mktemp -d)}; TL=${TL:-60}; P=${P:-14}
MODES=${MODES:-"none antecedent full"}
DROP=${DROP:-debug_triggers,mpu}
mkdir -p $OUT
[ -s $OUT/core.mlir ] || $H/import_core.sh $OUT/core.mlir $DROP 2>$OUT/import.err
[ -s $OUT/asserts.txt ] || $H/lower.sh $OUT/core.mlir --list \
  --pre-cache $OUT/pre.none > $OUT/asserts.txt 2>$OUT/list.err
# Word-level cycles are split. The core still has a cycle of bits (wb_stage
# <-> controller_fsm) that is only well defined in reachable states:
# synth-remove-false-cycles rebuilds it as its unique fixed point and adds an
# output that is 1 where the fixed point is not unique. That output is split
# off into its own model (aigsel.py); the model's result is only valid if
# rIC3 proves the obligation (UNSAT), else it is reported as INVALID.
EXPORT="hw.module(hw-cone-of-influence,canonicalize,hw-aggregate-to-comb,canonicalize,comb-split-word-cycles,canonicalize,convert-comb-to-synth,synth-remove-false-cycles{obligation fixpoint-obligation},synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash)"
export H OUT RIC3 TL CIRCT_PY EXPORT

lower() { a=$1 m=$2 f=$OUT/$1.$2
  opt=""; [ $m != none ] && opt="--odc $m"
  $H/lower.sh $OUT/core.mlir $f.mlir --assert $a $opt \
    --pre-cache $OUT/pre.$m > $f.log 2>&1 ||
    { echo "lower failed $a $m" >&2; return; }
  [ -s $f.mlir ] || return 0  # proved by verif-simplify-odc
  circt-opt $f.mlir --pass-pipeline="builtin.module($EXPORT)" \
    --mlir-pass-statistics 2>$f.export.log |
    circt-translate --export-aiger --exclude-symbol-table -o $f.full.aig \
      2>>$f.export.log || { echo "export failed $a $m" >&2; return; }
  if [ "$(head -1 $f.full.aig | cut -d' ' -f5)" = 2 ]; then
    python3 $H/aigsel.py $f.full.aig $f.aig 0 &&
      python3 $H/aigsel.py $f.full.aig $f.obl 1 && rm $f.full.aig
  else mv $f.full.aig $f.aig; fi; }
solve() { f=$1
  s=$(date +%s.%N)
  r=$(timeout $TL $RIC3 check $f --ui false ic3 --rseed 0 2>/dev/null | grep -aoE '^(SAT|UNSAT)$' | tail -1)
  printf "%s\t%s\t%.1f\n" $f "${r:-TO}" "$(echo "$(date +%s.%N)-$s" | bc)"; }
export -f lower solve

# The circt-opt part of the lowering is shared by all assertions of a mode:
# run it once per mode first. Lowering the core takes a few GB: LP jobs.
for m in $MODES; do echo $(head -1 $OUT/asserts.txt) $m; done |
  xargs -P 3 -L1 bash -c 'lower "$@"' _
for a in $(cat $OUT/asserts.txt); do for m in $MODES; do echo $a $m; done; done |
  xargs -P ${LP:-6} -L1 bash -c 'lower "$@"' _
# Solve each distinct AIG (models and obligations) once.
for f in $OUT/*.obl; do [ -e $f ] && mv $f $f.aig; done
md5sum $OUT/*.aig 2>/dev/null > $OUT/md5.txt
sort $OUT/md5.txt | awk '!seen[$1]++ {print $2}' |
  xargs -P $P -I{} bash -c 'solve {}' > $OUT/solved.tsv
result() { sum=$(awk -v f=$1 '$2==f {print $1}' $OUT/md5.txt)
  rep=$(sort $OUT/md5.txt | awk -v s=$sum '$1==s {print $2; exit}')
  awk -F'\t' -v r=$rep '$1==r {print $'$2'; exit}' $OUT/solved.tsv; }
printf "assert\tmode\tlatches\tands\tobligation\tresult\tseconds\n"
for a in $(cat $OUT/asserts.txt); do for m in $MODES; do f=$OUT/$a.$m
  if grep -q "^$a: proved by verif-simplify-odc" $f.log 2>/dev/null; then
    printf "%s\t%s\t0\t0\t-\tPROVED\t0\n" $a $m; continue; fi
  [ -s $f.aig ] || { printf "%s\t%s\t\t\t\tFAIL\t\n" $a $m; continue; }
  obl=-; [ -s $f.obl.aig ] && obl=$(result $f.obl.aig 2) && obl=${obl:-TO}
  r=$(result $f.aig 2); r=${r:-TO}
  [ $obl = - ] || [ $obl = UNSAT ] || r=INVALID
  printf "%s\t%s\t%s\t%s\t%s\t%s\n" $a $m \
    "$(head -1 $f.aig | awk '{print $4"\t"$6}')" $obl $r "$(result $f.aig 3)"
done; done
