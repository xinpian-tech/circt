#!/usr/bin/env bash
# Rocket-chip TinyConfig: one model per sampled Chisel assert, through each
# firtool optimization level x CIRCT synthesis configuration, then solvers.
# Output TSV: assert label opt config engine ands latches result seconds
set -u
H=$(cd "$(dirname "$0")" && pwd)
source $H/../env.sh
ABC=${ABC:-/root/yjh-workspace/notes/microexp/tools/abc-verifier/bin/abc}
RIC3=${RIC3:-/var/storage/workspaces/clo91eaf/yjh-workspace/rIC3-target-bare2/release/ric3}
IN=${IN:-/var/storage/workspaces/clo91eaf/yjh-workspace/rocket-mlir}
OUT=${OUT:-$IN/run1}
TL=${TL:-60}; P=${P:-12}; N=${N:-40}; SEED=${SEED:-1}
OPTS=${OPTS:-"release debug"}
CFGS=${CFGS:-"raw canon synth synth-area synth+dc2"}
ENGS=${ENGS:-"ric3-ic3 ric3-bare abc-pdr"}
mkdir -p $OUT
BITS='synth-lower-variadic,synth-lower-word-to-bits'

# lower MODEL CONFIG OUT.aig
lower() {
  local m=$1 c=$2 o=$3
  if [[ $c == *+dc2 ]]; then
    lower $m ${c%+dc2} $o.pre.aig &&
      $ABC -c "read $o.pre.aig; strash; dc2; write_aiger -s $o" >/dev/null
    return
  fi
  case $c in
    # raw: lowering plus structural hashing (every solver hashes on input)
    raw) circt-opt $m --pass-pipeline="builtin.module(hw.module(hw-aggregate-to-comb,convert-comb-to-synth,$BITS,synth-structural-hash))" ;;
    canon) circt-opt $m --pass-pipeline="builtin.module(canonicalize,cse,hw.module(hw-aggregate-to-comb,convert-comb-to-synth,$BITS,synth-structural-hash))" ;;
    synth) circt-synth $m ;;
    synth-area) circt-synth $m --synthesis-strategy=area ;;
  esac | circt-translate --export-aiger --exclude-symbol-table -o $o
}

solve() {
  local f=$1 e=$2
  case $e in
    ric3-ic3) timeout $TL $RIC3 check $f --ui false ic3 --rseed 0 2>/dev/null | grep -aoE '^(SAT|UNSAT)$' | tail -1 ;;
    ric3-bare) timeout $TL $RIC3 check $f --ui false --fe-simp false --fe-cnf plain ic3 --rseed 0 --preproc false 2>/dev/null | grep -aoE '^(SAT|UNSAT)$' | tail -1 ;;
    abc-pdr) timeout $TL $ABC -c "read $f; pdr" 2>&1 | grep -aoE 'Property proved|asserted' | head -1 | sed 's/Property proved/UNSAT/; s/asserted/SAT/' ;;
  esac
}

one() {
  local k=$1 o=$2 c=$3 e=$4
  local f=$OUT/a$k.$o.$c.aig
  local hdr; hdr=$(head -1 $f 2>/dev/null)
  local s; s=$(date +%s.%N)
  local r; r=$(solve $f $e)
  printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%.2f\n" $k "$(awk -F'\t' '{n[$2]++; print $2 "#" n[$2]}' $IN/labels.release.tsv | sed -n "$((k+1))p")" $o $c $e \
    "$(awk '{print $6}' <<<"$hdr")" "$(awk '{print $4}' <<<"$hdr")" "${r:-TO}" "$(echo "$(date +%s.%N)-$s" | bc)"
}
export -f lower solve one; export ABC RIC3 IN OUT TL BITS

# Sample asserts (indices into labels.release.tsv; the debug build has the
# same labels for the same asserts).
TOTAL=$(wc -l < $IN/labels.release.tsv)
KS=${KS:-$(python3 -c "import random; r=random.Random($SEED); print(' '.join(map(str, sorted(r.sample(range($TOTAL), $N)))))")}
echo "asserts: $KS" >&2
for o in $OPTS; do
  [ -f $IN/labels.$o.tsv ] || $CIRCT_PY $H/prep.py $IN/th.$o.mlir /dev/null --labels $IN/labels.$o.tsv >/dev/null
done
# Labels repeat across instances (every TLMonitor has assert__assert_2), so
# an assert is identified by (label, occurrence number in flattening order).
occ() { awk -F'\t' '{n[$2]++; print $1 "\t" $2 "#" n[$2]}' $1; }
for k in $KS; do
  lab=$(occ $IN/labels.release.tsv | sed -n "$((k+1))p" | cut -f2)
  for o in $OPTS; do
    ko=$(occ $IN/labels.$o.tsv | awk -F'\t' -v l="$lab" '$2==l{print $1; exit}')
    [ -n "$ko" ] || { echo "no $lab in $o" >&2; continue; }
    $CIRCT_PY $H/prep.py $IN/th.$o.mlir $OUT/a$k.$o.mlir --asserts $ko >/dev/null
    for c in $CFGS; do echo "$k $o $c"; done
  done
done | xargs -P $P -L1 bash -c 'lower $OUT/a$0.$1.mlir $2 $OUT/a$0.$1.$2.aig || echo "lower failed $0 $1 $2" >&2'
for k in $KS; do for o in $OPTS; do for c in $CFGS; do for e in $ENGS; do
  [ -f $OUT/a$k.$o.$c.aig ] && echo $k $o $c $e
done; done; done; done | xargs -P $P -L1 bash -c 'one "$@"' _
