#!/usr/bin/env bash
# Check that the tagged lowering of attr_run.sh gives the model of the final
# pipeline: run sc (merging) on <c>.l.mlir, export, compare &scl with A3.
#   validate.sh design check...
set -u
O=/root/yjh-workspace/solver-oriented-out; D=$O/study32/attr
export LD_LIBRARY_PATH=/nix/store/41ym1jm1b7j3rhglk82gwg9jml26z1km-gcc-14.3.0-lib/lib
B=/root/yjh-workspace/circt-solver-oriented/build/bin; A=/root/yjh-workspace/sb/solver-benchmark/solver/abc/abc
H=/root/yjh-workspace/circt-solver-oriented/experiments
BITS="synth-lower-word-to-bits,canonicalize,convert-comb-to-synth,synth-lower-word-to-bits,canonicalize,synth-remove-false-cycles{obligation fixpoint-obligation},synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash"
d=$1; shift
for c in "$@"; do (
  t=$(mktemp -d)
  $B/circt-opt $D/$d/$c.l.mlir --pass-pipeline="builtin.module(hw.module($BITS,synth-register-correspondence{internal},canonicalize,synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash))" -o $t/sc.mlir 2>/dev/null
  $B/circt-translate --export-aiger --exclude-symbol-table $t/sc.mlir -o $t/full.aig && python3 $H/synth-solver/cv32e40x/aigsel.py $t/full.aig $t/sc.aig 0
  n=$($A -c "read $t/sc.aig; strash; &get; &scl; &ps" | sed 's/\x1b\[[0-9;]*m//g' | grep -o "ff = *[0-9]*\|and = *[0-9]*" | tr -s ' ' | paste -sd' ')
  echo "$d $c tagged: $n | A3: $(awk '{print "and = "$1" ff = "$2}' $O/study32/raw/$d.$c.sc)"
  rm -rf $t ) & done; wait
