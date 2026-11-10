#!/usr/bin/env bash
# Each CIRCT synthesis configuration x each solver on the gen.py models.
# Output TSV: family width config engine ands tw result seconds
set -u
H=$(cd "$(dirname "$0")" && pwd)
CIRCT=${CIRCT:-/nix/store/j6xqv5appwkdk7xpw4mk2xxb5ydcb3xj-circt-1.147.0/bin}
ABC=${ABC:-/root/yjh-workspace/notes/microexp/tools/abc-verifier/bin/abc}
RIC3=${RIC3:-/var/storage/workspaces/clo91eaf/yjh-workspace/rIC3-target-bare2/release/ric3}
TW=${TW:-/root/yjh-workspace/aigopt/target/release/aigtw}
OUT=${OUT:-/var/storage/workspaces/clo91eaf/yjh-workspace/microexp-data/synth-solver}
TL=${TL:-30}; P=${P:-12}
FAMS=${FAMS:-"mulcomm hoist exch xorintro reorder shift counter"}
WS=${WS:-"4 8 12"}
CFGS=${CFGS:-"raw canon lower synth synth-area synth-nodp synth-fr synth-sop raw+dc2 synth+dc2"}
ENGS=${ENGS:-"ric3-ic3 ric3-bare ric3-kind abc-pdr"}
mkdir -p "$OUT"
BITS='synth-lower-variadic,synth-lower-word-to-bits'

# lower MODEL CONFIG OUT: MLIR -> AIGER through one configuration
lower() {
  local m=$1 c=$2 o=$3
  if [[ $c == *+dc2 ]]; then
    lower $m ${c%+dc2} $o.pre.aig &&
      $ABC -c "read $o.pre.aig; strash; dc2; write_aiger -s $o" >/dev/null
    return
  fi
  if [[ $c == sc* ]]; then
    # solvercanon.py, then raw lowering (sc) or circt-synth (sc+synth);
    # sc-noRULE switches one rule off.
    local flags=""; [[ $c == sc-no* ]] && flags="--no-${c#sc-no}"
    python3 $H/solvercanon.py --check $flags $m $o.sc.mlir || return 1
    if [[ $c == sc+synth ]]; then lower $o.sc.mlir synth $o; else lower $o.sc.mlir raw $o; fi
    return
  fi
  case $c in
    raw) $CIRCT/circt-opt $m --pass-pipeline="builtin.module(hw.module(convert-comb-to-synth,$BITS))" ;;
    canon) $CIRCT/circt-opt $m --pass-pipeline="builtin.module(canonicalize,cse,hw.module(convert-comb-to-synth,$BITS))" ;;
    lower) $CIRCT/circt-synth $m --until-after=comb-lowering |
      $CIRCT/circt-opt --pass-pipeline="builtin.module(hw.module($BITS))" ;;
    synth) $CIRCT/circt-synth $m ;;
    synth-area) $CIRCT/circt-synth $m --synthesis-strategy=area ;;
    synth-nodp) $CIRCT/circt-synth $m --disable-datapath ;;
    synth-fr) $CIRCT/circt-synth $m --enable-functional-reduction ;;
    synth-sop) $CIRCT/circt-synth $m --enable-sop-balancing ;;
  esac | $CIRCT/circt-translate --export-aiger -o $o
}

solve() {
  local f=$1 e=$2
  case $e in
    ric3-ic3) timeout $TL $RIC3 check $f --ui false ic3 --rseed 0 2>/dev/null | grep -aoE '^(SAT|UNSAT)$' | tail -1 ;;
    ric3-bare) timeout $TL $RIC3 check $f --ui false --fe-simp false --fe-cnf plain ic3 --rseed 0 --preproc false 2>/dev/null | grep -aoE '^(SAT|UNSAT)$' | tail -1 ;;
    ric3-kind) timeout $TL $RIC3 check $f --ui false kind 2>/dev/null | grep -aoE '^(SAT|UNSAT)$' | tail -1 ;;
    abc-pdr) timeout $TL $ABC -c "read $f; pdr" 2>&1 | grep -aoE 'Property proved|asserted' | head -1 | sed 's/Property proved/UNSAT/; s/asserted/SAT/' ;;
  esac
}

one() {
  local fam=$1 w=$2 c=$3 e=$4
  local f=$OUT/$fam-$w.$c.aig
  local ands; ands=$(head -1 $f | awk '{print $6}')
  local tw; tw=$($TW $f 2>/dev/null | awk '{print $4}' | head -1)
  local s; s=$(date +%s.%N)
  local r; r=$(solve $f $e)
  printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%.2f\n" $fam $w $c $e "$ands" "${tw:--}" "${r:-TO}" "$(echo "$(date +%s.%N)-$s" | bc)"
}
export -f lower solve one; export CIRCT ABC RIC3 TW OUT TL BITS H

# build models
for fam in $FAMS; do for w in $WS; do
  python3 $H/gen.py $fam $w > $OUT/$fam-$w.mlir
  for c in $CFGS; do lower $OUT/$fam-$w.mlir $c $OUT/$fam-$w.$c.aig || echo "lower failed: $fam $w $c" >&2; done
done; done
for fam in $FAMS; do for w in $WS; do for c in $CFGS; do for e in $ENGS; do echo $fam $w $c $e; done; done; done; done |
  xargs -P $P -L1 bash -c 'one "$@"' _
