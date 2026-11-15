#!/usr/bin/env bash
# seg.sh NAME FROM TO [FROM2 TO2]: sc on the unshared hl model merging only
# classes [FROM,TO) ; rBMC time to depth 20.
S=/tmp/claude-0/-root-yjh-workspace/3cca2a40-f56d-411e-8bea-05778e44e84e/scratchpad; Y=/var/storage/workspaces/clo91eaf/yjh-workspace
export LD_LIBRARY_PATH=/nix/store/41ym1jm1b7j3rhglk82gwg9jml26z1km-gcc-14.3.0-lib/lib; B=$S/build/bin; cd $S/t/bis
M=${MODEL:-a_shift_result_hl}
BITS="synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash"
n=$1; f=$2; t=$3
$B/circt-opt $M.us.mlir --pass-pipeline="builtin.module(hw.module(synth-register-correspondence{internal merge-from=$f merge-to=$t},canonicalize,$BITS))" --mlir-pass-statistics -o $n.mlir 2>$n.log &&
 $B/circt-translate --export-aiger --exclude-symbol-table $n.mlir -o $n.full.aig && python3 $S/exp-wt/experiments/synth-solver/cv32e40x/aigsel.py $n.full.aig $n.aig 0; rm -f $n.mlir $n.full.aig
echo "$n [$f,$t) $(head -1 $n.aig | awk '{print $4"/"$6}') merged=$(grep -E 'num-merged ' $n.log | awk '{print $2}')+$(grep -E 'num-merged-gates' $n.log | awk '{print $2}')"
