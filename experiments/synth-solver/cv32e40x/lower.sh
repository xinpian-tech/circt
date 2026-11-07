#!/usr/bin/env bash
# Lower circt-verilog output to a safety model for AIGER export, with CIRCT
# passes only:
#
#   lower.sh IN.mlir OUT.mlir [--assert L1,L2] [--assume L3,L4]
#            [--odc full|antecedent] [--pre-cache PREFIX]
#   lower.sh IN.mlir --list [--odc full|antecedent] [--pre-cache PREFIX]
#
# PRE (independent of the assertion, cached with --pre-cache): flatten, LLHD
# processes and signals to registers and logic (llhd-combine-drives fills
# undriven parts of signals without initial value with any value), SVA to
# LTL to core, aggregates to integers; with --odc, verif-simplify-odc first.
# MODEL: the selected assertions (by SVA label; default all) and all
# assumptions become one output `bad` (verif-assertions-to-output), seq.firreg
# becomes seq.compreg with an initial value (registers with reset start in
# reset; others with any value), symbolic values become inputs.
#
# Assertions proved by verif-simplify-odc are removed by it: for those,
# "LABEL: proved by verif-simplify-odc" is printed and no model is written.
# Assertions folded to true during PRE (gone without a remark) print
# "LABEL: folded to true by the lowering".
set -euo pipefail
in=$1; shift
out=
if [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; then out=$1; shift; fi
asserts= assumes= odc= cache= list=
while [ $# -gt 0 ]; do
  case $1 in
    --assert) asserts=$2; shift 2 ;;
    --assume) assumes=$2; shift 2 ;;
    --odc) odc=$2; shift 2 ;;
    --pre-cache) cache=$2; shift 2 ;;
    --list) list=1; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

case $odc in
  "") ODC= ;;
  full) ODC=",verif-simplify-odc,canonicalize" ;;
  antecedent) ODC=",verif-simplify-odc{antecedent-only},canonicalize" ;;
  *) echo "--odc full|antecedent" >&2; exit 2 ;;
esac
PRE="builtin.module(hw-flatten-modules,symbol-dce,hw.module(llhd-mem2reg,llhd-hoist-signals,llhd-deseq,llhd-lower-processes,canonicalize,llhd-remove-control-flow,canonicalize,llhd-sig2reg,llhd-combine-drives{free-undriven},llhd-sig2reg,canonicalize$ODC,lower-ltl-to-core,lower-seq-shiftreg,lower-seq-compreg-ce),cse,hw-flatten-io,hw.module(hw-aggregate-to-comb,canonicalize),cse)"

tmp=$(mktemp -d)
trap 'rm -rf $tmp' EXIT
pre=${cache:-$tmp/pre}
if [ ! -s $pre.mlir ]; then
  circt-opt $in --pass-pipeline="$PRE" -o $pre.mlir.tmp 2>$pre.err.tmp ||
    { cat $pre.err.tmp >&2; exit 1; }
  mv $pre.err.tmp $pre.err; mv $pre.mlir.tmp $pre.mlir
fi
labels() { (grep -oE 'verif\.(clocked_)?assert .* label "[^"]+"' "$1" || true) |
  sed -E 's/.*label "([^"]+)".*/\1/' | sed '/^$/d' | sort -u; }
proved=$( (grep -oE "remark: property holds under its don't-cares \(label '[^']+'\)" \
  $pre.err || true) | sed -E "s/.*label '([^']+)'.*/\1/" | sort -u)
# Assertions of the input that PRE removed without a remark were folded to
# true by canonicalize (an assert of a constant true, e.g. after
# verif-simplify-odc and lower-ltl-to-core): they hold too.
folded=$(comm -23 <(labels $in) <(labels $pre.mlir) | comm -23 - <(echo "$proved"))
proved=$(printf '%s\n%s\n' "$proved" "$folded" | sed '/^$/d' | sort -u)
if [ -n "$list" ]; then
  { labels $pre.mlir; echo "$proved"; } | sed '/^$/d' | sort -u
  exit 0
fi
[ -n "$out" ] || { echo "no output file" >&2; exit 2; }

# Selected assertions proved by verif-simplify-odc have no model.
left=
for l in ${asserts//,/ }; do
  if grep -qxF "$l" <<<"$folded"; then echo "$l: folded to true by the lowering"
  elif grep -qxF "$l" <<<"$proved"; then echo "$l: proved by verif-simplify-odc"
  else left=$left${left:+,}$l; fi
done
if [ -n "$asserts" ] && [ -z "$left" ]; then exit 0; fi

opts=
[ -n "$left" ] && opts="asserts=$left"
[ -n "$assumes" ] && opts="$opts${opts:+ }assumes=$assumes"
MODEL="builtin.module(verif-assertions-to-output{$opts},hw.module(seq-lower-firreg-to-compreg),verif-lower-symbolic-values{mode=hw-input},canonicalize,cse,hw-convert-bitcasts,canonicalize,hw-convert-bitcasts,canonicalize,cse)"
circt-opt $pre.mlir --pass-pipeline="$MODEL" -o $out
if grep -qE '\bllhd\.' $out; then
  echo "warning: LLHD ops left: $(grep -oE '\bllhd\.[a-z_.]+' $out | sort -u | tr '\n' ' ')" >&2
fi
