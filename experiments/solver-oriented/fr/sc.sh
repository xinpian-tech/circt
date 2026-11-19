#!/usr/bin/env bash
# sc.sh TAG : signal correspondence (export_cfgs SC) on TAG.mlir; size and time -> TAG.sc.res
S=$(cd $(dirname $0) && pwd); R=/root/yjh-workspace/circt-solver-oriented; B=$R/build/bin
A=/root/yjh-workspace/sb/solver-benchmark/solver/abc/abc
export LD_LIBRARY_PATH=/nix/store/41ym1jm1b7j3rhglk82gwg9jml26z1km-gcc-14.3.0-lib/lib
T=$S/$1
BITS="synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash"
s=$(date +%s.%N)
$B/circt-opt $T.mlir --pass-pipeline="builtin.module(hw.module(synth-register-correspondence{internal},canonicalize,$BITS))" -o $T.sc.mlir 2>$T.sc.log || { echo FAIL; exit 1; }
e=$(date +%s.%N)
$B/circt-translate --export-aiger --exclude-symbol-table $T.sc.mlir -o $T.sc.full.aig && python3 $R/experiments/synth-solver/cv32e40x/aigsel.py $T.sc.full.aig $T.sc.aig 0 && rm -f $T.sc.full.aig $T.sc.mlir
sz=$($A -c "read $T.sc.aig; strash; &get; &scl; &ps" | sed 's/\x1b\[[0-9;]*m//g' | awk '/ff =/{for(i=1;i<=NF;i++){if($i=="ff")f=$(i+2); if($i=="and")a=$(i+2)} printf "%s/%s", a, f}')
printf "%s SC %s (%.1fs)\n" $1 "$sz" $(echo "$e-$s"|bc) | tee $T.sc.res
