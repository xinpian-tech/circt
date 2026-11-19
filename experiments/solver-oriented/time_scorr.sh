#!/usr/bin/env bash
# Time of the solvers' own signal correspondence on BASE and WL models:
# ABC `scorr` (on the plain model, as the ABC back ends read it) and rIC3's
# pairwise scorr and functional reduction (from its log).
#   OUT=dir P=8 time_scorr.sh
set -u
O=/root/yjh-workspace/solver-oriented-out; SB=/root/yjh-workspace/sb/solver-benchmark
OUT=${OUT:?}; P=${P:-8}; mkdir -p $OUT/plain
export O SB OUT
one() { local d=$1 a=$2 c=$3 f=$4 pl=$OUT/plain/$1.$2.$3.aig
  [ -s $pl ] || python3 -I $SB/experiment/aigplain.py $f $pl
  local s=$(date +%s.%N)
  local st=$(timeout 1800 $SB/solver/abc/abc -c "read $pl; strash; scorr; print_stats" 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -oE "lat = +[0-9]+ +and = +[0-9]+" | tail -1 | tr -s ' ' | sed 's/lat = //; s/ and = /\t/')
  local ta=$(echo "$(date +%s.%N)-$s" | bc)
  local log=$(RUST_LOG=info timeout 300 $SB/solver/rIC3/rIC3 check $f --ui false ic3 --rseed 0 2>&1 | grep -E "scorr: eliminates|frts: eliminates")
  local rs=$(grep -oE "scorr: eliminates [0-9]+ latchs out of [0-9]+ in [0-9.]+s" <<<"$log" | grep -oE "[0-9.]+" | tr '\n' ' ')
  local rf=$(grep -oE "frts: eliminates [0-9]+ out of [0-9]+ vars in [0-9.]+s" <<<"$log" | grep -oE "[0-9.]+" | tr '\n' ' ')
  printf "%s\t%s\t%s\t%.1f\t%s\t%s\t%s\n" $d $a $c $ta "${st:-NA	NA}" "$rs" "$rf"; }
export -f one
{
for a in $(ls $O/core-env-final/*.sc.aig | xargs -n1 basename | sed 's/\.sc\.aig$//'); do
  echo cv32 $a base $O/core-env-final/$a.base.aig; echo cv32 $a ws3r $O/bench/cv32/ws3r/$a.base.aig; done
for a in $(ls $O/bench/picou/base/*.sc.aig | xargs -n1 basename | sed 's/\.sc\.aig$//'); do
  echo pico $a base $O/bench/picou/base/$a.base.aig; echo pico $a ws3r $O/bench/picou/ws3r/$a.base.aig; done
} | xargs -P $P -L1 bash -c 'one "$@"' _ > $OUT/scorr.tsv
echo done
