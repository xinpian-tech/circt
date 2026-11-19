#!/usr/bin/env bash
# Experiment 1: wall time of the lowering with each cumulative set of
# word-level steps (no AIGER export, no sc), per check, one process each.
#   OUT=dir P=4 time_cfgs.sh
set -u
R=/root/yjh-workspace/circt-solver-oriented; O=/root/yjh-workspace/solver-oriented-out
export LD_LIBRARY_PATH=/nix/store/41ym1jm1b7j3rhglk82gwg9jml26z1km-gcc-14.3.0-lib/lib
OUT=${OUT:?}; P=${P:-4}; mkdir -p $OUT
CS="comb-context-simplify{depth=32 budget=16384},canonicalize,cse,"
CS2="comb-context-simplify{depth=32 budget=16384 or-form cases=4},canonicalize,cse,"
KB="seq-known-bits{context},canonicalize,cse,"
TRIM="seq-trim-registers,canonicalize,cse,"
AGG="hw-aggregate-to-comb,canonicalize,cse,"
WC="seq-word-congruence,canonicalize,cse,"
lower() { # DCP ADDER
  local cts=convert-comb-to-synth; [ -n "$2" ] && cts="convert-comb-to-synth{adder-architecture=$2}"
  echo "builtin.module(hw.module(canonicalize,cse,hw-cone-of-influence,canonicalize,hw-aggregate-to-comb,canonicalize,$1synth-lower-word-to-bits,canonicalize,$cts,synth-lower-word-to-bits,canonicalize,synth-remove-false-cycles{obligation fixpoint-obligation},synth-lower-variadic,synth-lower-word-to-bits,synth-structural-hash))"; }
export R OUT
declare -A PIPE=(
  [base]="$(lower '' '')"
  [r]="$(lower '' RIPPLE-CARRY)"
  [kb]="$(lower "$KB$TRIM" RIPPLE-CARRY)"
  [nowc]="$(lower "$KB$CS$CS2$TRIM$AGG" RIPPLE-CARRY)"
  [ws3r]="$(lower "$KB$CS$CS2$WC$TRIM$AGG" RIPPLE-CARRY)"
)
jobs=()
for a in $(ls $O/core-env-final/*.sc.aig | xargs -n1 basename | sed 's/\.sc\.aig$//'); do
  for c in base r kb nowc ws3r; do jobs+=("cv32 $a $c /var/storage/workspaces/clo91eaf/yjh-workspace/synth-solver-out/core-env/$a.none.mlir"); done; done
for a in $(ls $O/picorv32/none-u/*.none.mlir | xargs -n1 basename | sed 's/\.none\.mlir$//'); do
  for c in base r kb nowc ws3r; do jobs+=("pico $a $c $O/picorv32/none-u/$a.none.mlir"); done; done
for c in base r kb nowc ws3r; do printf '%s\t%s\n' $c "${PIPE[$c]}"; done > $OUT/pipelines.tsv
one() { local d=$1 a=$2 c=$3 in=$4; local p; p=$(awk -F'\t' -v c=$c '$1==c{print $2}' $OUT/pipelines.tsv)
  local s=$(date +%s.%N)
  $R/build/bin/circt-opt $in --pass-pipeline="$p" -o /dev/null 2>/dev/null
  printf "%s\t%s\t%s\t%.2f\n" $d $a $c $(echo "$(date +%s.%N)-$s" | bc); }
export -f one
printf "design\tcheck\tconfig\tseconds\n" > $OUT/times.tsv
printf '%s\n' "${jobs[@]}" | xargs -P $P -L1 bash -c 'one "$@"' _ >> $OUT/times.tsv
echo done
