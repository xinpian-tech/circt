#!/usr/bin/env bash
# CIRCT front-end configurations measured by BMC with rBMC (a fixed back end:
# AIG to CNF by Tseitin only, incremental CaDiCaL).
#   IN="dir..." CFGS="base canon ..." K=20 TL=300 P=12 run_rbmc.sh A... > out.tsv
# For each assertion A and configuration C, the model A.C.aig (first found in
# the IN directories, e.g. from run_aig_configs.sh) is checked up to bound K
# with the time limit TL (SIGINT, then SIGKILL 5 s later). Output TSV on
# stdout: assert config result depth seconds witness, where result is
#   SAT  counterexample of length depth (witness: aigsim -c ok/BAD),
#   K    no counterexample up to bound K (depth = K),
#   TO   time limit (depth = last bound checked without counterexample),
# and summarize_rbmc.py compares the configurations.
set -u
RBMC=${RBMC:-/var/storage/workspaces/clo91eaf/yjh-workspace/rbmc-bundle/rbmc}
AIGSIM=${AIGSIM:-/var/storage/workspaces/clo91eaf/yjh-workspace/rbmc-bundle/aigsim}
IN=${IN:?}; CFGS=${CFGS:?}; K=${K:-20}; TL=${TL:-300}; P=${P:-12}
WORK=${WORK:-$(mktemp -d)}
export RBMC AIGSIM IN K TL WORK

one() {
  local a=$1 c=$2 f="" d
  for d in $IN; do [ -s $d/$a.$c.aig ] && { f=$d/$a.$c.aig; break; }; done
  [ -n "$f" ] || { printf "%s\t%s\tMISSING\t\t\t\n" $a $c; return; }
  local o=$WORK/$a.$c s=$(date +%s.%N)
  timeout -s INT -k 5 $TL $RBMC $f --end $K --witness > $o.out 2> $o.err
  local t=$(echo "$(date +%s.%N)-$s" | bc)
  local r=TO dep w=-
  if head -1 $o.out | grep -q '^SAT$'; then
    r=SAT; dep=$(grep -oE 'length [0-9]+' $o.err | grep -oE '[0-9]+')
    tail -n +2 $o.out > $o.wit
    $AIGSIM -c $f $o.wit > /dev/null 2>&1 && w=ok || w=BAD
  else
    dep=$(grep -oE 'up to bound [0-9]+' $o.err | grep -oE '[0-9]+')
    [ "$dep" = "$K" ] && r=K
  fi
  printf "%s\t%s\t%s\t%s\t%.2f\t%s\n" $a $c $r "${dep:-}" $t $w
}
export -f one

for a in "$@"; do for c in $CFGS; do echo $a $c; done; done |
  xargs -P $P -L1 bash -c 'one "$@"' _
