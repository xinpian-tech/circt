#!/usr/bin/env bash
# Models of several configurations on the back ends of the benchmark plan.
#   IN=dir OUT=dir CFGS="base sc" BACKENDS="..." K=20 TL=150 P=12 run_matrix.sh A...
# IN holds A.CFG.aig (AIGER 1.9: one output, initial values, constraints).
# Output TSV on stdout: assert config backend result depth seconds, where
# result is SAT (depth: counterexample length), K (no counterexample up to
# bound K), UNSAT (proved) or TO (depth: last bound without counterexample,
# for the bounded back ends).
# Back ends:
#   rbmc           rBMC (Tseitin only, incremental CaDiCaL)
#   ric3-bmc       rIC3 bmc with its preprocessing
#   ric3-ic3-bare  rIC3 ic3, no preprocessing, plain CNF
#   ric3-ic3       rIC3 ic3 with its preprocessing
#   abc-bmc3, abc-pdr          ABC on the model as it is
#   abc-pre-bmc3, abc-pre-pdr  ABC after scorr, fraig, dc2 (time included)
# ABC reads the model rewritten by aigplain.py (latches start at 0, the
# constraints folded into the output).
set -u
H=$(cd "$(dirname "$0")" && pwd)
Y=/var/storage/workspaces/clo91eaf/yjh-workspace
RBMC=${RBMC:-$Y/rbmc-bundle/rbmc}
RIC3=${RIC3:-$Y/rIC3-target-rebuild-20261106/release/ric3-bundled}
ABC=${ABC:-abc}
IN=${IN:?}; OUT=${OUT:?}; CFGS=${CFGS:-"base sc"}; K=${K:-20}; TL=${TL:-150}; P=${P:-12}
BACKENDS=${BACKENDS:-"rbmc ric3-bmc ric3-ic3-bare ric3-ic3 abc-bmc3 abc-pdr abc-pre-bmc3 abc-pre-pdr"}
mkdir -p $OUT
export H RBMC RIC3 ABC IN OUT K TL

one() {
  local a=$1 c=$2 e=$3 f=$IN/$1.$2.aig o=$OUT/$1.$2.$3
  [ -s $f ] || { printf "%s\t%s\t%s\tMISSING\t\t\n" $a $c $e; return; }
  local run="timeout -s INT -k 5 $TL" pl=$OUT/$a.$c.plain.aig
  case $e in abc-*) [ -s $pl ] || python3 $H/aigplain.py $f $pl.$e && mv -n $pl.$e $pl; rm -f $pl.$e ;; esac
  local s=$(date +%s.%N)
  case $e in
    rbmc) $run $RBMC $f --end $K ;;
    ric3-bmc) $run $RIC3 check $f --ui false bmc --end $K ;;
    ric3-ic3-bare) $run $RIC3 check $f --ui false --fe-simp false --fe-cnf plain ic3 --preproc false ;;
    ric3-ic3) $run $RIC3 check $f --ui false ic3 ;;
    abc-bmc3) $run $ABC -c "read $pl; bmc3 -F $((K + 1)) -T $TL" ;;
    abc-pdr) $run $ABC -c "read $pl; pdr -T $TL" ;;
    abc-pre-bmc3) $run $ABC -c "read $pl; scorr; fraig; dc2; bmc3 -F $((K + 1)) -T $TL" ;;
    abc-pre-pdr) $run $ABC -c "read $pl; scorr; fraig; dc2; pdr -T $TL" ;;
  esac > $o.log 2>&1
  local t=$(echo "$(date +%s.%N)-$s" | bc) r=TO d=
  case $e in
    rbmc)
      if grep -aq '^SAT$' $o.log; then r=SAT; d=$(grep -aoE 'length [0-9]+' $o.log | grep -oE '[0-9]+' | tail -1)
      else d=$(grep -aoE 'up to bound [0-9]+' $o.log | grep -oE '[0-9]+' | tail -1); [ "$d" = "$K" ] && r=K; fi ;;
    ric3-bmc)
      if grep -aq '^SAT$' $o.log; then r=SAT
      else d=$(grep -aoE 'up to depth [0-9]+' $o.log | grep -oE '[0-9]+' | tail -1); [ "$d" = "$K" ] && r=K; fi ;;
    ric3-ic3*) r=$(grep -aoE '^(SAT|UNSAT)$' $o.log | tail -1); r=${r:-TO} ;;
    abc-*bmc3)
      if grep -aq 'was asserted in frame' $o.log; then r=SAT; d=$(grep -aoE 'asserted in frame [0-9]+' $o.log | grep -oE '[0-9]+')
      else d=$(grep -aoE 'No output asserted in [0-9]+ frames' $o.log | grep -oE '[0-9]+' | tail -1)
        [ -n "$d" ] && { d=$((d - 1)); [ $d -ge $K ] && r=K; }; fi ;;
    abc-*pdr)
      grep -aq 'Property proved' $o.log && r=UNSAT
      grep -aq 'was asserted in frame' $o.log && { r=SAT; d=$(grep -aoE 'asserted in frame [0-9]+' $o.log | grep -oE '[0-9]+'); } ;;
  esac
  printf "%s\t%s\t%s\t%s\t%s\t%.2f\n" $a $c $e $r "${d:-}" $t
}
export -f one

printf "assert\tconfig\tbackend\tresult\tdepth\tseconds\n"
for a in "$@"; do for c in $CFGS; do for e in $BACKENDS; do echo $a $c $e; done; done; done |
  xargs -P $P -L1 bash -c 'one "$@"' _
