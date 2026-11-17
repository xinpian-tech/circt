#!/usr/bin/env bash
# Export tc, fr and tcfr models (and, where unsharing changes the model,
# ustc, usfr, ustcfr) next to the models of export_cfgs.sh.
#   CIRCT_BIN=dir IN=dir OUT=dir P=12 export_parts.sh A...
set -u
H=$(cd "$(dirname "$0")" && pwd)
B=${CIRCT_BIN:?}; IN=${IN:?}; OUT=${OUT:?}; P=${P:-12}
PRE="canonicalize,cse"
LOWER="hw-cone-of-influence,canonicalize,hw-aggregate-to-comb,canonicalize,synth-lower-word-to-bits,canonicalize,convert-comb-to-synth,synth-lower-word-to-bits,canonicalize,synth-remove-false-cycles{obligation fixpoint-obligation},synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash"
BITS="synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash"
TC="synth-ternary-constants,canonicalize,$BITS"
FR="synth-functional-reduction{use-choices=false},canonicalize,$BITS"
export H B IN OUT PRE LOWER TC FR
one() {
  local a=$1
  toaig() { $B/circt-translate --export-aiger --exclude-symbol-table $1 -o $2.full.aig &&
    python3 $H/../synth-solver/cv32e40x/aigsel.py $2.full.aig $2 0 && rm -f $2.full.aig; }
  gen() { # prefix-of-config pre-passes
    local p=$1 pre=$2
    [ -s $OUT/$a.${p}tcfr.aig ] && return
    $B/circt-opt $IN/$a.none.mlir --pass-pipeline="builtin.module(hw.module($PRE,$pre$LOWER))" -o $OUT/$a.${p}x.mlir 2>/dev/null
    for c in tc fr tcfr; do
      case $c in tc) q=$TC;; fr) q=$FR;; tcfr) q="$TC,$FR";; esac
      local s=$(date +%s.%N)
      $B/circt-opt $OUT/$a.${p}x.mlir --pass-pipeline="builtin.module(hw.module($q))" -o $OUT/$a.$p$c.mlir 2>$OUT/$a.$p$c.log && toaig $OUT/$a.$p$c.mlir $OUT/$a.$p$c.aig
      printf "%.1f\n" "$(echo "$(date +%s.%N)-$s" | bc)" > $OUT/$a.$p$c.time
      rm -f $OUT/$a.$p$c.mlir
    done
    rm -f $OUT/$a.${p}x.mlir
  }
  gen "" ""
  [ -e $OUT/$a.us.same ] || gen us "comb-unshare-operators,$PRE,"
  echo "$a $(for c in tc fr tcfr; do head -1 $OUT/$a.$c.aig | awk '{printf "%s/%s ", $4, $6}'; done) times $(cat $OUT/$a.tc.time) $(cat $OUT/$a.fr.time) $(cat $OUT/$a.tcfr.time)"
}
export -f one
for a in "$@"; do echo $a; done | xargs -P $P -I{} bash -c 'one {}'
