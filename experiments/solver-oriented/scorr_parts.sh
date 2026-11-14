#!/usr/bin/env bash
# Where the reduction of signal correspondence comes from, measured with
# ABC on AIGER models: constants by ternary simulation, combinational
# equivalence, register correspondence, full signal correspondence.
#   TL=100 P=3 scorr_parts.sh MODEL.aig... > out.tsv
# Output TSV: model step latches ands seconds (empty sizes: time limit).
# Steps: strash; scl (scleanup); scl+fraig; lcorr+fraig (scorr -l; fraig);
# scorr; scorr+fraig. ABC reads latches without initial value as 0.
set -u
ABC=${ABC:-abc}; TL=${TL:-100}; P=${P:-3}
export ABC TL
one() {
  local f=$1 n; n=$(basename $f .aig)
  step() {
    local s=$(date +%s.%N) r
    r=$(timeout $TL $ABC -c "read $f; strash; $2; print_stats" 2>&1 |
      grep -oE 'lat = *[0-9]+ +and = *[0-9]+' | tail -1 | grep -oE '[0-9]+' | tr '\n' '\t')
    printf "%s\t%s\t%s%.1f\n" $n $1 "${r:-$'\t\t'}" "$(echo "$(date +%s.%N)-$s" | bc)"
  }
  step strash ""; step scl "scleanup"; step scl+fraig "scleanup; fraig"
  step lcorr+fraig "scorr -l; fraig"; step scorr "scorr"; step scorr+fraig "scorr; fraig"
}
export -f one
printf "model\tstep\tlatches\tands\tseconds\n"
printf "%s\n" "$@" | xargs -P $P -I{} bash -c 'one {}'
