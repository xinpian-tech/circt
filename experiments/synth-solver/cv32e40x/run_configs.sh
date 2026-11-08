#!/usr/bin/env bash
# Synthesis configurations x solvers on ex_stage assertion models.
#   run_configs.sh MODELDIR ASSERT...
# MODELDIR holds ASSERT.mlir from lower.sh (run_ex_stage.sh writes them).
# Output TSV on stdout: assert config engine latches ands result seconds
set -u
H=$(cd "$(dirname "$0")" && pwd)
source $H/../env.sh
RIC3=${RIC3:-/var/storage/workspaces/clo91eaf/yjh-workspace/rIC3-target-rebuild-20261106/release/ric3-bundled}
SC=$H/../solvercanon.py
IN=$1; shift
OUT=${OUT:-$IN/configs}; TL=${TL:-300}; P=${P:-14}
CFGS=${CFGS:-"raw canon synth synth-area sc sc+synth"}
ENGS=${ENGS:-"ric3-ic3 ric3-bare"}
mkdir -p $OUT
BITS='synth-lower-variadic,synth-lower-word-to-bits'
RAW="hw.module(hw-aggregate-to-comb,convert-comb-to-synth,$BITS,synth-structural-hash)"

# lower MODEL.mlir CONFIG OUT.aig
lower() {
  local m=$1 c=$2 o=$3
  case $c in
    sc|sc+synth) python3 $SC $m $o.sc.mlir || return 1; m=$o.sc.mlir; c=${c#sc}; c=${c#+}; c=${c:-raw} ;;
  esac
  case $c in
    raw) circt-opt $m --pass-pipeline="builtin.module($RAW)" ;;
    canon) circt-opt $m --pass-pipeline="builtin.module(canonicalize,cse,$RAW)" ;;
    synth) circt-synth $m ;;
    synth-area) circt-synth $m --synthesis-strategy=area ;;
    synth-nodp) circt-synth $m --disable-datapath ;;
  esac | circt-translate --export-aiger --exclude-symbol-table -o $o
}

solve() {
  local f=$1 e=$2
  case $e in
    ric3-ic3) timeout $TL $RIC3 check $f --ui false ic3 --rseed 0 2>/dev/null ;;
    ric3-bare) timeout $TL $RIC3 check $f --ui false --fe-simp false --fe-cnf plain ic3 --rseed 0 --preproc false 2>/dev/null ;;
  esac | grep -aoE '^(SAT|UNSAT)$' | tail -1
}

one() {
  local a=$1 c=$2 e=$3 f=$OUT/$1.$2.aig
  local s r; s=$(date +%s.%N); r=$(solve $f $e)
  printf "%s\t%s\t%s\t%s\t%s\t%.1f\n" $a $c $e "$(head -1 $f | awk '{print $4"\t"$6}')" "${r:-TO}" \
    "$(echo "$(date +%s.%N)-$s" | bc)"
}
export -f lower solve one; export RIC3 SC OUT TL RAW

for a in "$@"; do for c in $CFGS; do echo $a $c; done; done |
  xargs -P $P -L1 bash -c 'lower $0/$1.mlir $2 $OUT/$1.$2.aig 2>$OUT/$1.$2.err || echo "lower failed $1 $2" >&2' "$IN"
printf "assert\tconfig\tengine\tlatches\tands\tresult\tseconds\n"
for a in "$@"; do for c in $CFGS; do for e in $ENGS; do
  [ -s $OUT/$a.$c.aig ] && echo $a $c $e
done; done; done | xargs -P $P -L1 bash -c 'one "$@"' _
