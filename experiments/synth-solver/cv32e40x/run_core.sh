#!/usr/bin/env bash
# Whole CV32E40X core: one model per SVA assertion, lowered without and with
# verif-simplify-odc (antecedent-only and full), exported to AIGER and checked
# with rIC3 IC3. Identical AIGs (the pass changed nothing) are solved once.
#   OUT=dir TL=60 P=14 MODES="none antecedent full" run_core.sh
# Output TSV on stdout: assert mode latches ands cuts result seconds
set -u
H=$(cd "$(dirname "$0")" && pwd)
source $H/../env.sh
RIC3=${RIC3:-/var/storage/workspaces/clo91eaf/yjh-workspace/rIC3-target-rebuild-20261106/release/ric3}
OUT=${OUT:-$(mktemp -d)}; TL=${TL:-60}; P=${P:-14}
MODES=${MODES:-"none antecedent full"}
DROP=${DROP:-debug_triggers,mpu}
mkdir -p $OUT
[ -s $OUT/core.mlir ] || $H/import_core.sh $OUT/core.mlir $DROP 2>$OUT/import.err
[ -s $OUT/asserts.txt ] || $CIRCT_PY $H/lower_regs.py $OUT/core.mlir /dev/null \
  --top cv32e40x_core_top --list > $OUT/asserts.txt 2>$OUT/list.err
# Word-level cycles are split; the remaining cycles of bits are cut by free
# inputs (an over-approximation: UNSAT stays valid, SAT needs checking).
EXPORT="hw.module(hw-cone-of-influence,canonicalize,hw-aggregate-to-comb,canonicalize,comb-split-word-cycles{cut-remaining},canonicalize,convert-comb-to-synth,synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash)"
export H OUT RIC3 TL CIRCT_PY EXPORT

lower() { a=$1 m=$2 f=$OUT/$1.$2
  opt=""; [ $m != none ] && opt="--odc $m"
  $CIRCT_PY $H/lower_regs.py $OUT/core.mlir $f.mlir --top cv32e40x_core_top \
    --assert $a $opt > $f.log 2>&1 || { echo "lower failed $a $m" >&2; return; }
  circt-opt $f.mlir --pass-pipeline="builtin.module($EXPORT)" \
    --mlir-pass-statistics 2>$f.export.log |
    circt-translate --export-aiger --exclude-symbol-table -o $f.aig 2>>$f.export.log ||
    echo "export failed $a $m" >&2; }
solve() { f=$1
  s=$(date +%s.%N)
  r=$(timeout $TL $RIC3 check $f --ui false ic3 --rseed 0 2>/dev/null | grep -aoE '^(SAT|UNSAT)$' | tail -1)
  printf "%s\t%s\t%.1f\n" $f "${r:-TO}" "$(echo "$(date +%s.%N)-$s" | bc)"; }
export -f lower solve

for a in $(cat $OUT/asserts.txt); do for m in $MODES; do echo $a $m; done; done |
  xargs -P $P -L1 bash -c 'lower "$@"' _
# Solve each distinct AIG once.
md5sum $OUT/*.aig 2>/dev/null > $OUT/md5.txt
sort $OUT/md5.txt | awk '!seen[$1]++ {print $2}' |
  xargs -P $P -I{} bash -c 'solve {}' > $OUT/solved.tsv
printf "assert\tmode\tlatches\tands\tcuts\tresult\tseconds\n"
for a in $(cat $OUT/asserts.txt); do for m in $MODES; do f=$OUT/$a.$m
  if grep -q "^$a: proved by verif-simplify-odc" $f.log 2>/dev/null; then
    printf "%s\t%s\t0\t0\t0\tPROVED\t0\n" $a $m; continue; fi
  [ -s $f.aig ] || { printf "%s\t%s\t\t\t\tFAIL\t\n" $a $m; continue; }
  sum=$(awk -v f=$f.aig '$2==f {print $1}' $OUT/md5.txt)
  rep=$(sort $OUT/md5.txt | awk -v s=$sum '$1==s {print $2; exit}')
  cuts=$(grep -oE '[0-9]+ num-cuts' $f.export.log | cut -d' ' -f1)
  printf "%s\t%s\t%s\t%s\t%s\t%s\n" $a $m "$(head -1 $f.aig | awk '{print $4"\t"$6}')" \
    "${cuts:-0}" "$(awk -F'\t' -v r=$rep '$1==r {print $2"\t"$3}' $OUT/solved.tsv)"
done; done
