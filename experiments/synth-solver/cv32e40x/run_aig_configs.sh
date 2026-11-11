#!/usr/bin/env bash
# CIRCT AIG simplifications on the whole-core models, measured by the rIC3
# solve time of the exported AIG.
#   IN=dir OUT=dir CFGS="base fr ..." SEEDS="0 1 2" TL=60 P=8 run_aig_configs.sh A...
# IN holds the lowered models A.none.mlir (run_core.sh). Each model is
# exported once up to the AIG (BASE, as in run_core.sh), then each
# configuration runs its passes on that AIG and exports it. Output TSV on
# stdout: assert config latches ands pass_seconds seed result seconds.
# The obligation output of synth-remove-false-cycles is split off (aigsel.py)
# and not solved here: the configurations keep the function of each output,
# so it is the same for all of them (run_core.sh checks it).
set -u
H=$(cd "$(dirname "$0")" && pwd)
source $H/../env.sh
RIC3=${RIC3:-/var/storage/workspaces/clo91eaf/yjh-workspace/rIC3-target-rebuild-20261106/release/ric3-bundled}
ABC=${ABC:-abc}
IN=${IN:?}; OUT=${OUT:?}; TL=${TL:-60}; P=${P:-8}; SEEDS=${SEEDS:-"0 1 2"}
CFGS=${CFGS:-"base canon mac sop fr opt opt-nosop opt-fr abc-dc2 abc-fraig abc-scorr"}
mkdir -p $OUT
BASE="hw.module(hw-cone-of-influence,canonicalize,hw-aggregate-to-comb,canonicalize,synth-lower-word-to-bits,canonicalize,convert-comb-to-synth,synth-lower-word-to-bits,canonicalize,synth-remove-false-cycles{obligation fixpoint-obligation},synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash)"
BITS="synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash"
export H RIC3 ABC IN OUT TL BASE BITS

# The passes of a configuration, run on the base AIG (MLIR), or an ABC
# script run on the exported base AIG (external reference points).
pipeline() {
  case $1 in
    base) echo "" ;;
    canon) echo "hw.module(canonicalize,cse,$BITS)" ;;
    mac) echo "hw.module(synth-maximum-and-cover,$BITS)" ;;
    sop) echo "hw.module(synth-sop-balancing{max-cut-input-size=4 max-cuts-per-root=4},$BITS)" ;;
    sop6) echo "hw.module(synth-sop-balancing{max-cut-input-size=6 max-cuts-per-root=8},$BITS)" ;;
    # Functional reduction (simulation + SAT, like ABC's fraig); without
    # choices, which ExportAIGER cannot write, each node is replaced by its
    # representative.
    fr) echo "hw.module(synth-functional-reduction{use-choices=false},canonicalize,$BITS)" ;;
    # circt-synth's default optimization pipeline (buildSynthOptimizationPipeline,
    # not registered in circt-opt), without and with functional reduction.
    opt) echo "hw.module(cse,synth-structural-hash,canonicalize,synth-maximum-and-cover,$BITS,synth-sop-balancing{max-cut-input-size=4 max-cuts-per-root=4},$BITS)" ;;
    opt-fr) echo "hw.module(cse,synth-structural-hash,canonicalize,synth-maximum-and-cover,$BITS,synth-sop-balancing{max-cut-input-size=4 max-cuts-per-root=4},$BITS,synth-functional-reduction{use-choices=false},canonicalize,$BITS)" ;;
    # Register correspondence (sequential: registers equal, complementary or
    # constant in all reachable states are merged).
    rc) echo "hw.module(synth-register-correspondence,canonicalize,$BITS)" ;;
    # Signal correspondence (and-gates too), ternary constants, the register
    # split of synth-forward-retime (unread bits of wide registers go), and
    # all of them followed by functional reduction.
    sc) echo "hw.module(synth-register-correspondence{internal},canonicalize,$BITS)" ;;
    # The same, assuming in the inductive step that the property held before
    # (the module's output is the bad state).
    rcp) echo "hw.module(synth-register-correspondence{property-directed bad-outputs conjunction},canonicalize,$BITS)" ;;
    scp) echo "hw.module(synth-register-correspondence{internal property-directed bad-outputs conjunction},canonicalize,$BITS)" ;;
    tc) echo "hw.module(synth-ternary-constants,canonicalize,$BITS)" ;;
    split) echo "hw.module(synth-forward-retime,canonicalize,$BITS)" ;;
    all) echo "hw.module(synth-forward-retime,synth-ternary-constants,canonicalize,synth-register-correspondence{internal},canonicalize,$BITS,synth-functional-reduction{use-choices=false},canonicalize,$BITS)" ;;
    # Without sop balancing.
    opt-nosop) echo "hw.module(cse,synth-structural-hash,canonicalize,synth-maximum-and-cover,$BITS)" ;;
  esac
}
abcscript() {
  case $1 in
    abc-dc2) echo "strash; dc2" ;;
    abc-fraig) echo "strash; fraig" ;;
    abc-scorr) echo "strash; scorr; dc2" ;;
    *) echo "" ;;
  esac
}
export -f pipeline abcscript

# export A CFG: the model of assertion A under configuration CFG.
export_one() {
  local a=$1 c=$2 b=$OUT/$1.base.mlir o=$OUT/$1.$2
  local s=$(date +%s.%N)
  case $c in
    abc-*)
      $ABC -c "read $OUT/$a.base.aig; $(abcscript $c); write_aiger -s $o.aig" \
        > $o.log 2>&1 ;;
    base) cp $OUT/$a.base.aig $o.aig ;;
    *)
      circt-opt $b --pass-pipeline="builtin.module($(pipeline $c))" \
        --mlir-pass-statistics 2>$o.log |
        circt-translate --export-aiger --exclude-symbol-table -o $o.full.aig \
          2>>$o.log &&
        python3 $H/aigsel.py $o.full.aig $o.aig 0 && rm $o.full.aig ;;
  esac
  printf "%.1f\n" "$(echo "$(date +%s.%N)-$s" | bc)" > $o.time
}
solve_one() {
  local f=$1 seed=$2 s=$(date +%s.%N)
  local r=$(timeout $TL $RIC3 check $f --ui false ic3 --rseed $seed 2>/dev/null |
    grep -aoE '^(SAT|UNSAT)$' | tail -1)
  printf "%s\t%s\t%s\t%.1f\n" $f $seed "${r:-TO}" \
    "$(echo "$(date +%s.%N)-$s" | bc)"
}
export -f export_one solve_one

# Base AIG of each assertion (MLIR and exported model).
for a in "$@"; do echo $a; done | xargs -P ${LP:-4} -I{} bash -c '
  a={}; b=$OUT/$a.base.mlir
  [ -s $OUT/$a.base.aig ] && exit 0
  echo 0 > $OUT/$a.base.time
  circt-opt $IN/$a.none.mlir --pass-pipeline="builtin.module($BASE)" \
    -o $b 2>$OUT/$a.base.log &&
    circt-translate --export-aiger --exclude-symbol-table $b -o $OUT/$a.base.full.aig &&
    python3 $H/aigsel.py $OUT/$a.base.full.aig $OUT/$a.base.aig 0 ||
    echo "base failed $a" >&2'
for a in "$@"; do for c in $CFGS; do
  [ -s $OUT/$a.base.aig ] && [ ! -s $OUT/$a.$c.aig ] && echo $a $c
done; done | xargs -P ${LP:-4} -L1 bash -c 'export_one "$@"' _
for a in "$@"; do for c in $CFGS; do for s in $SEEDS; do
  [ -s $OUT/$a.$c.aig ] && echo $OUT/$a.$c.aig $s
done; done; done | xargs -P $P -L1 bash -c 'solve_one "$@"' _ > $OUT/solved.tsv

printf "assert\tconfig\tlatches\tands\tpass_seconds\tseed\tresult\tseconds\n"
for a in "$@"; do for c in $CFGS; do f=$OUT/$a.$c.aig
  if [ ! -s $f ]; then printf "%s\t%s\t\t\t\t\tFAIL\t\n" $a $c; continue; fi
  awk -F'\t' -v f=$f -v a=$a -v c=$c -v h="$(head -1 $f | awk '{print $4"\t"$6}')" \
    -v t="$(cat $OUT/$a.$c.time)" \
    '$1==f {printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n", a, c, h, t, $2, $3, $4}' \
    $OUT/solved.tsv
done; done
