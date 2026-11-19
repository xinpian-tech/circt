#!/usr/bin/env bash
# gen.sh OUT.mlir IN.none.mlir [WS] [ADDER] : bit-level model (export_cfgs LOWER)
R=/root/yjh-workspace/circt-solver-oriented; B=$R/build/bin
export LD_LIBRARY_PATH=/nix/store/41ym1jm1b7j3rhglk82gwg9jml26z1km-gcc-14.3.0-lib/lib
O=$1; IN=$2; WS=${3:-0}; ADDER=${4:-}
DCP=""
RFC=${RFC:-"synth-remove-false-cycles{obligation fixpoint-obligation}"}
CS="comb-context-simplify{depth=32 budget=16384},canonicalize,cse,"
CS2="comb-context-simplify{depth=32 budget=16384 or-form cases=4},canonicalize,cse,"
[ "$WS" = 2 ] && DCP="seq-known-bits{context},canonicalize,cse,$CS${CS2}seq-word-congruence,canonicalize,cse,seq-trim-registers,canonicalize,cse,hw-aggregate-to-comb,canonicalize,cse,"
CTS=convert-comb-to-synth; [ -n "$ADDER" ] && CTS="convert-comb-to-synth{adder-architecture=$ADDER}"
LOWER="hw-cone-of-influence,canonicalize,hw-aggregate-to-comb,canonicalize,${DCP}synth-lower-word-to-bits,canonicalize,$CTS,synth-lower-word-to-bits,canonicalize,$RFC,synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash"
s=$(date +%s.%N)
$B/circt-opt $IN --pass-pipeline="builtin.module(hw.module(canonicalize,cse,$LOWER))" -o $O 2>$O.err || { echo FAIL $O; tail -3 $O.err; exit 1; }
echo "$O $(echo "$(date +%s.%N)-$s"|bc)s"
