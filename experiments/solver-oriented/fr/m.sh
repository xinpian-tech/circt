#!/usr/bin/env bash
# m.sh TAG IN.mlir [FR-OPTIONS] : FR on a bit-level model; sizes (ABC &scl) of
# IN, IN+&fraig, IN+FR; FR pass time. Writes TAG.res
S=$(cd $(dirname $0) && pwd); R=/root/yjh-workspace/circt-solver-oriented; B=$R/build/bin
A=/root/yjh-workspace/sb/solver-benchmark/solver/abc/abc
export LD_LIBRARY_PATH=/nix/store/41ym1jm1b7j3rhglk82gwg9jml26z1km-gcc-14.3.0-lib/lib
T=$S/$1; IN=$2; O=${3:-}
BITS="synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash"
toaig() { $B/circt-translate --export-aiger --exclude-symbol-table $1 -o $2.full.aig && python3 $R/experiments/synth-solver/cv32e40x/aigsel.py $2.full.aig $2 0 && rm -f $2.full.aig; }
size() { sed 's/\x1b\[[0-9;]*m//g' | awk '/ff =/{for(i=1;i<=NF;i++){if($i=="ff")f=$(i+2); if($i=="and")a=$(i+2)} printf "%s/%s ", a, f}'; }
toaig $IN $T.in.aig
s=$(date +%s.%N)
$B/circt-opt $IN --pass-pipeline="builtin.module(hw.module(synth-functional-reduction{use-choices=false $O},canonicalize,$BITS))" --mlir-timing --mlir-pass-statistics -o $T.fr.mlir 2> $T.fr.log || { echo FAIL; tail -3 $T.fr.log; exit 1; }
e=$(date +%s.%N)
frt=$(awk '/%) +FunctionalReduction$/{print $1; exit}' $T.fr.log)
toaig $T.fr.mlir $T.fr.aig
base=$($A -c "read $T.in.aig; strash; &get; &scl; &ps" | size)
s2=$(date +%s.%N)
fraig=$($A -c "read $T.in.aig; strash; &get; &scl; &fraig -C 1000; &scl; &ps" | size)
e2=$(date +%s.%N)
fr=$($A -c "read $T.fr.aig; strash; &get; &scl; &ps" | size)
frf=$($A -c "read $T.fr.aig; strash; &get; &scl; &fraig -C 1000; &scl; &ps" | size)
printf "%s base %s fraig %s(%.1fs) FR %s(pass %.2fs, run %.1fs) FR+fraig %s\n" $1 "$base" "$fraig" $(echo "$e2-$s2"|bc) "$fr" $frt $(echo "$e-$s"|bc) "$frf" | tee $T.res
