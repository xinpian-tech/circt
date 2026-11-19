#!/usr/bin/env bash
# cec.sh TAG IN.mlir [FR-OPTIONS] : ABC cec of IN and IN+FR, registers cut
S=$(cd $(dirname $0) && pwd); W=$S/../wcg; R=/root/yjh-workspace/circt-solver-oriented; B=$R/build/bin
export LD_LIBRARY_PATH=/nix/store/41ym1jm1b7j3rhglk82gwg9jml26z1km-gcc-14.3.0-lib/lib
T=$S/$1.cec; IN=$2; O=${3:-}
$B/circt-opt $IN --pass-pipeline="builtin.module(hw.module(synth-functional-reduction{use-choices=false $O}))" -o $T.post.mlir || exit 1
L="canonicalize,hw-aggregate-to-comb,canonicalize,synth-lower-word-to-bits,canonicalize,convert-comb-to-synth,synth-lower-word-to-bits,canonicalize,synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash"
for x in pre post; do
  src=$IN; [ $x = post ] && src=$T.post.mlir
  if [ -n "${CYC:-}" ]; then python3 $S/cyccut.py cut $src $T.$x.cc.mlir 2>/dev/null; src=$T.$x.cc.mlir; fi
  python3 $W/cut.py $src $T.$x.cut.mlir
  $B/circt-opt $T.$x.cut.mlir --pass-pipeline="builtin.module(hw.module($L))" -o $T.$x.bits.mlir || exit 1
  $B/circt-translate --export-aiger --exclude-symbol-table $T.$x.bits.mlir -o $T.$x.aig || exit 1
done
echo "$1 $(/root/yjh-workspace/.gcroots/abc/bin/abc -c "cec $T.pre.aig $T.post.aig" | tail -1)"
rm -f $T.*.mlir
