#!/usr/bin/env bash
# Per check: word-level IR as in the final pipeline (up to the point where the
# word-level passes would go), every op line tagged with loc("w":LINE:0),
# lowered to bits with the final pipeline (default adders) and signal
# correspondence run with merge-dump, (a) without register initial values
# (noinit: every proved class holds in every state, i.e. combinational
# merges) and (b) on the normal model.
#   attr_run.sh  (all checks of both designs; P parallel processes)
# Output: study32/attr/<design>/<check>.{l.mlir,noinit.md,init.md,*.time}
set -u
S=$(cd "$(dirname "$0")" && pwd); . $S/checks.sh
export LD_LIBRARY_PATH=/nix/store/41ym1jm1b7j3rhglk82gwg9jml26z1km-gcc-14.3.0-lib/lib
export B=/root/yjh-workspace/circt-solver-oriented/build/bin D=$O/study32/attr
WORD="canonicalize,cse,hw-cone-of-influence,canonicalize,hw-aggregate-to-comb,canonicalize"
BITS="synth-lower-word-to-bits,canonicalize,convert-comb-to-synth,synth-lower-word-to-bits,canonicalize,synth-remove-false-cycles{obligation fixpoint-obligation},synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash"
export WORD BITS
word() { # design check in
  local o=$D/$1/$2
  [ -s $o.l.mlir ] && return
  $B/circt-opt $3/$2.none.mlir --pass-pipeline="builtin.module(hw.module($WORD))" -o $o.w.mlir 2>$o.w.err || return
  # Tag every op line (an SSA result) with its line number.
  awk '/^ *%[^ ]* = /&&!/loc\(/&&!/\{ *$/{sub(/[ ]*$/,""); $0=$0" loc(\"w\":" NR ":0)"} {print}' $o.w.mlir > $o.l.mlir
  # Same lines without the registers' initial values.
  sed -E '/seq\.compreg/s/ initial %[A-Za-z0-9_.$-]+//' $o.l.mlir > $o.n.mlir
  rm -f $o.w.mlir
}
sc() { # design check variant(noinit|init)
  local o=$D/$1/$2 f
  [ -s $o.$3.md ] && return
  if [ $3 = noinit ]; then f=$o.n.mlir; else f=$o.l.mlir; fi
  local s=$(date +%s.%N)
  $B/circt-opt $f --mlir-print-debuginfo --mlir-pass-statistics \
    --pass-pipeline="builtin.module(hw.module($BITS,synth-register-correspondence{internal merge-dump merge-to=0}))" \
    -o /dev/null 2> $o.$3.tmp
  echo "$? $(echo "$(date +%s.%N)-$s" | bc)" > $o.$3.time
  # The class lines (merge-to=0: the classes are printed, none is merged, so
  # the per-merge cone descriptions, slow on large models, are skipped) and
  # the statistics.
  grep -E "^class |num-|error" $o.$3.tmp > $o.$3.md; rm -f $o.$3.tmp
}
export -f word sc
mkdir -p $D/cv32 $D/pico
{ for c in $CVA; do echo cv32 $c $CVIN; done; for c in $PA; do echo pico $c $PIN; done; } |
  xargs -P ${P:-12} -L1 bash -c 'word "$@"' _
# PicoRV32 first (fast), then CV32E40X in a fixed random order (seed 32), so
# that the checks finished at any point are a seeded random sample; both
# variants of a check are adjacent.
CVS=$(echo $CVA | tr ' ' '\n' | python3 -c 'import random,sys; l=sys.stdin.read().split(); random.Random(32).shuffle(l); print(" ".join(l))')
echo "$CVS" > $D/cv32.order
{ for c in $PA; do echo pico $c noinit; echo pico $c init; done
  for c in $CVS; do echo cv32 $c noinit; echo cv32 $c init; done; } |
  xargs -P ${P:-12} -L1 bash -c 'sc "$@"' _
echo attr done
