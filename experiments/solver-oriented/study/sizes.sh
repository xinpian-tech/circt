#!/usr/bin/env bash
# Measure every model of the study with the bundled ABC: after `&scl`
# (cone of influence, dangling latches), after `&fraig -C 1000` (combinational SAT
# sweeping, at most 1000 conflicts per node: the default 1e6 does not finish
# on the multiplier checks) and after a second `&scl` (latches left dangling by &fraig).
# Writes study32/raw/<design>.<check>.<cfg>: "and ff" for scl, fraig, fraig+scl.
set -u
S=$(cd "$(dirname "$0")" && pwd); . $S/checks.sh
A=/root/yjh-workspace/sb/solver-benchmark/solver/abc/abc
D=$O/study32; mkdir -p $D/raw
one() { # design check cfg file
  local out=$D/raw/$1.$2.$3
  [ -s $out ] && return
  timeout 1800 $A -c "read $4; strash; &get; &scl; &ps; &fraig -C 1000; &ps; &scl; &ps" 2>&1 | sed 's/\x1b\[[0-9;]*m//g' |
    awk '/ff =/{for(i=1;i<=NF;i++){if($i=="ff")f=$(i+2); if($i=="and")a=$(i+2)} printf "%s %s ", a, f} END{print ""}' > $out.tmp && mv $out.tmp $out
}
export -f one; export A D
{
for c in $CVA; do
  echo cv32 $c base $O/core-env-final/$c.base.aig
  echo cv32 $c sc $O/core-env-final/$c.sc.aig
  echo cv32 $c kbd $D/cv32/kbd/$c.base.aig
  echo cv32 $c ws3 $O/bench/cv32/ws3/$c.base.aig
done
for c in $PA; do
  echo pico $c base $O/bench/pico/base/$c.base.aig
  echo pico $c sc $O/bench/pico/base/$c.sc.aig
  echo pico $c kbd $D/pico/kbd/$c.base.aig
  echo pico $c ws3 $O/bench/pico/ws3/$c.base.aig
done
} | xargs -P ${P:-12} -L1 bash -c 'one "$@"' _
echo sizes done
