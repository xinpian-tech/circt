#!/usr/bin/env bash
# Which part of rIC3's own front end still helps on a model?  Runs rIC3 ic3
# with the front end fully on, fully off, each item alone and each item
# removed, optionally under several random seeds.
#   IN=dir OUT=dir CFG=ussc VARIANTS="on off ..." SEEDS="0" TL=150 P=12 \
#     ric3_frontend.sh A...
# Output TSV on stdout: assert variant seed result seconds.
# Items: simp (--fe-simp), cnf (--fe-cnf compact instead of plain), scorr,
# frts, bve (the three steps of ic3 --preproc).  Variant names: on, off,
# only-ITEM, no-ITEM.
set -u
Y=/var/storage/workspaces/clo91eaf/yjh-workspace
RIC3=${RIC3:-$Y/rIC3-target-rebuild-20261106/release/ric3-bundled}
IN=${IN:?}; OUT=${OUT:?}; CFG=${CFG:-ussc}; TL=${TL:-150}; P=${P:-12}
VARIANTS=${VARIANTS:-"on off only-simp only-cnf only-scorr only-frts only-bve no-simp no-cnf no-scorr no-frts no-bve"}
SEEDS=${SEEDS:-0}
mkdir -p $OUT
export RIC3 IN OUT CFG TL

one() {
  local a=$1 v=$2 s=$3 f=$IN/$1.$CFG.aig o=$OUT/$1.$CFG.fe-$2.s$3.log
  local simp=true cnf=compact scorr=true frts=true bve=true
  case $v in
    off | only-*) simp=false cnf=plain scorr=false frts=false bve=false ;;
  esac
  case $v in
    only-simp) simp=true ;; only-cnf) cnf=compact ;; only-scorr) scorr=true ;;
    only-frts) frts=true ;; only-bve) bve=true ;;
    no-simp) simp=false ;; no-cnf) cnf=plain ;; no-scorr) scorr=false ;;
    no-frts) frts=false ;; no-bve) bve=false ;;
  esac
  local pre=true
  [ $scorr$frts$bve = falsefalsefalse ] && pre=false
  local t0=$(date +%s.%N)
  timeout -s INT -k 5 $TL $RIC3 check $f --ui false --fe-simp $simp --fe-cnf $cnf \
    ic3 --rseed $s --preproc $pre --scorr $scorr --frts $frts --bve $bve > $o 2>&1
  local t=$(echo "$(date +%s.%N)-$t0" | bc) r
  r=$(grep -aoE '^(SAT|UNSAT)$' $o | tail -1)
  printf "%s\t%s\t%s\t%s\t%.2f\n" $a $v $s ${r:-TO} $t
}
export -f one

printf "assert\tvariant\tseed\tresult\tseconds\n"
for a in "$@"; do for v in $VARIANTS; do for s in $SEEDS; do echo $a $v $s; done; done; done |
  xargs -P $P -L1 bash -c 'one "$@"' _
