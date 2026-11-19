#!/usr/bin/env bash
# Export the models of the evaluation: base, us (comb-unshare-operators),
# sc (signal correspondence) and ussc (both), from lowered models
# IN/A.none.mlir.
#   CIRCT_BIN=dir IN=dir OUT=dir P=12 export_cfgs.sh A...
# When unsharing changes nothing, A.us.aig and A.ussc.aig are hard links to
# the base and sc models (A.us.same marks it).
# KB=1 adds seq-known-bits there (see below).
# DC=1 adds comb-mux-dont-care and seq-trim-registers after the aggregates
# are lowered (observability don't cares; every configuration gets them).
set -u
H=$(cd "$(dirname "$0")" && pwd)
B=${CIRCT_BIN:?}; IN=${IN:?}; OUT=${OUT:?}; P=${P:-12}
mkdir -p $OUT
PRE="canonicalize,cse"
DCP=""
[ "${DC:-0}" = 1 ] && DCP="comb-mux-dont-care,canonicalize,cse,seq-trim-registers,canonicalize,hw-cone-of-influence,"
# KB=1 adds seq-known-bits (constant register bits by abstract interpretation
# on words) at the same place.
[ "${KB:-0}" = 1 ] && DCP="${DCP}seq-known-bits,canonicalize,cse,"
# WS=1 adds the word-level simplification: path-sensitive known bits and
# context simplification (twice), word congruence with case splits, and
# trimming of the register bits nothing reads.
CS="comb-context-simplify{depth=32 budget=16384},canonicalize,cse,"
[ "${WS:-0}" = 1 ] && DCP="${DCP}seq-known-bits{context},canonicalize,cse,$CS${CS}seq-word-congruence,canonicalize,cse,seq-trim-registers,canonicalize,cse,"
# WS=2: the second context simplification with or-form and case splits on
# disjunctive facts (cases=4); aggregates the per-slice rewrite leaves are
# lowered again.
CS2="comb-context-simplify{depth=32 budget=16384 or-form cases=4},canonicalize,cse,"
[ "${WS:-0}" = 2 ] && DCP="${DCP}seq-known-bits{context},canonicalize,cse,$CS${CS2}seq-word-congruence,canonicalize,cse,seq-trim-registers,canonicalize,cse,hw-aggregate-to-comb,canonicalize,cse,"
# ADDER=RIPPLE-CARRY (or another architecture) for all adders and comparators.
# XDCP=passes, appended at the same place (ablations).
DCP="${DCP}${XDCP:-}"
CTS=convert-comb-to-synth
[ -n "${ADDER:-}" ] && CTS="convert-comb-to-synth{adder-architecture=$ADDER}"
LOWER="hw-cone-of-influence,canonicalize,hw-aggregate-to-comb,canonicalize,${DCP}synth-lower-word-to-bits,canonicalize,$CTS,synth-lower-word-to-bits,canonicalize,synth-remove-false-cycles{obligation fixpoint-obligation},synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash"
BITS="synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash"
SC="synth-register-correspondence{internal},canonicalize,$BITS"
export H B IN OUT PRE LOWER SC NOUS NOSC
one() {
  local a=$1
  toaig() { $B/circt-translate --export-aiger --exclude-symbol-table $1 -o $2.full.aig &&
    python3 $H/../synth-solver/cv32e40x/aigsel.py $2.full.aig $2 0 && rm -f $2.full.aig; }
  run() { local s=$(date +%s.%N); "$@"; printf "%.1f\n" "$(echo "$(date +%s.%N)-$s" | bc)"; }
  [ -s $OUT/$a.ussc.aig ] && return
  $B/circt-opt $IN/$a.none.mlir --pass-pipeline="builtin.module(hw.module($PRE,$LOWER))" -o $OUT/$a.base.mlir 2>$OUT/$a.base.log && toaig $OUT/$a.base.mlir $OUT/$a.base.aig
  # NOUS=1: no unsharing (us and ussc are the base and sc models).
  if [ "${NOUS:-0}" = 1 ]; then cp $OUT/$a.base.mlir $OUT/$a.us.mlir; cp $OUT/$a.base.aig $OUT/$a.us.aig; else
  $B/circt-opt $IN/$a.none.mlir --pass-pipeline="builtin.module(hw.module($PRE,comb-unshare-operators,$PRE,$LOWER))" --mlir-pass-statistics -o $OUT/$a.us.mlir 2>$OUT/$a.us.log && toaig $OUT/$a.us.mlir $OUT/$a.us.aig; fi
  # NOSC=1: only the base (and us) model; no sc (ablations).
  if [ "${NOSC:-0}" = 1 ]; then cp $OUT/$a.base.aig $OUT/$a.sc.aig; else
  run $B/circt-opt $OUT/$a.base.mlir --pass-pipeline="builtin.module(hw.module($SC))" -o $OUT/$a.sc.mlir 2>$OUT/$a.sc.log > $OUT/$a.sc.time && toaig $OUT/$a.sc.mlir $OUT/$a.sc.aig; fi
  if cmp -s $OUT/$a.base.aig $OUT/$a.us.aig; then
    ln -f $OUT/$a.base.aig $OUT/$a.us.aig; ln -f $OUT/$a.sc.aig $OUT/$a.ussc.aig; touch $OUT/$a.us.same
  elif [ "${NOSC:-0}" = 1 ]; then
    cp $OUT/$a.us.aig $OUT/$a.ussc.aig
  else
    $B/circt-opt $OUT/$a.us.mlir --pass-pipeline="builtin.module(hw.module($SC))" -o $OUT/$a.ussc.mlir 2>$OUT/$a.ussc.log && toaig $OUT/$a.ussc.mlir $OUT/$a.ussc.aig
  fi
  rm -f $OUT/$a.*.mlir
  echo "$a $([ -e $OUT/$a.us.same ] && echo same || echo changed) $(for c in base us sc ussc; do head -1 $OUT/$a.$c.aig | awk '{printf "%s/%s ", $4, $6}'; done)"
}
export -f one
for a in "$@"; do echo $a; done | xargs -P $P -I{} bash -c 'one {}'
