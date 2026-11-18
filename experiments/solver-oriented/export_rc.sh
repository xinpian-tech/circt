#!/usr/bin/env bash
# Register correspondence only (synth-register-correspondence without
# internal: registers, not and-gates) on the base model, as A.rc.aig, with
# the time of the pass in A.rc.time.
#   CIRCT_BIN=dir IN=dir OUT=dir P=12 export_rc.sh A...
set -u
H=$(cd "$(dirname "$0")" && pwd)
B=${CIRCT_BIN:?}; IN=${IN:?}; OUT=${OUT:?}; P=${P:-12}
mkdir -p $OUT
LOWER=$(grep '^LOWER=' $H/export_cfgs.sh | sed 's/^LOWER="//; s/"$//; s/\${DCP}//')
BITS="synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash"
export H B IN OUT LOWER BITS
one() {
  local a=$1 t
  [ -s $OUT/$a.rc.aig ] && return
  $B/circt-opt $IN/$a.none.mlir --pass-pipeline="builtin.module(hw.module(canonicalize,cse,$LOWER))" -o $OUT/$a.rcbase.mlir || return
  t=$(date +%s.%N)
  $B/circt-opt $OUT/$a.rcbase.mlir --pass-pipeline="builtin.module(hw.module(synth-register-correspondence,canonicalize,$BITS))" -o $OUT/$a.rc.mlir 2>$OUT/$a.rc.log
  printf "%.1f\n" "$(echo "$(date +%s.%N)-$t" | bc)" > $OUT/$a.rc.time
  $B/circt-translate --export-aiger --exclude-symbol-table $OUT/$a.rc.mlir -o $OUT/$a.rc.full.aig &&
    python3 $H/../synth-solver/cv32e40x/aigsel.py $OUT/$a.rc.full.aig $OUT/$a.rc.aig 0
  rm -f $OUT/$a.rc.full.aig $OUT/$a.rcbase.mlir $OUT/$a.rc.mlir
  echo "$a $(cat $OUT/$a.rc.time) $(head -1 $OUT/$a.rc.aig | awk '{print $4"/"$6}')"
}
export -f one
for a in "$@"; do echo $a; done | xargs -P $P -I{} bash -c 'one {}'
