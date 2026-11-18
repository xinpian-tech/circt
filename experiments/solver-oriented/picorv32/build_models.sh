#!/usr/bin/env bash
# Import and lower every riscv-formal check of PicoRV32 to OUT/A.none.mlir
# (A: check name without _ch0; with BUG=00N, A_b00N), ready for
# export_cfgs.sh.
#   OUT=dir P=12 [BUG=00N] build_models.sh [CHECK...]   (default: all but cover)
set -u
H=$(cd "$(dirname "$0")" && pwd)
source $H/../../synth-solver/env.sh
RF=${RF:-/root/yjh-workspace/ext/riscv-formal}
OUT=${OUT:?}; P=${P:-12}
mkdir -p $OUT
checks=("$@")
[ ${#checks[@]} -gt 0 ] || checks=($(ls $RF/cores/picorv32/checks | sed -n 's/_ch0\.sby$//p'))
export H OUT BUG=${BUG:-}
one() {
  local c=$1 a=${1}${BUG:+_b$BUG} sby
  [ -s $OUT/$a.none.mlir ] && return
  sby=${c}_ch0
  $H/import.sh $sby $OUT/$a.mlir 2>$OUT/$a.import.log &&
    $H/../../synth-solver/cv32e40x/lower.sh $OUT/$a.mlir $OUT/$a.none.mlir --immediate 2>$OUT/$a.lower.log
  echo "$a $([ -s $OUT/$a.none.mlir ] && echo ok || echo FAILED)"
  rm -f $OUT/$a.mlir
}
export -f one
printf '%s\n' "${checks[@]}" | xargs -P $P -I{} bash -c 'one {}'
