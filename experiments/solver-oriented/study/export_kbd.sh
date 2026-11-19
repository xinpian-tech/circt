#!/usr/bin/env bash
# Export the known-bits-only models ("kbd", default adders, no unsharing, no
# sc) of both designs for the Section 3.2 study:
#   seq-known-bits{context} + seq-trim-registers before bit-blasting.
set -u
R=/root/yjh-workspace/circt-solver-oriented; O=/root/yjh-workspace/solver-oriented-out
export LD_LIBRARY_PATH=/nix/store/41ym1jm1b7j3rhglk82gwg9jml26z1km-gcc-14.3.0-lib/lib CIRCT_BIN=$R/build/bin P=${P:-12}
E=$R/experiments/solver-oriented/export_cfgs.sh
. $(dirname "$0")/checks.sh
KB="seq-known-bits{context},canonicalize,cse,seq-trim-registers,canonicalize,cse,"
env NOUS=1 NOSC=1 XDCP="$KB" IN=$CVIN OUT=$O/study32/cv32/kbd $E $CVA > $O/study32/cv32.kbd.log 2>&1
env NOUS=1 NOSC=1 XDCP="$KB" IN=$PIN OUT=$O/study32/pico/kbd $E $PA > $O/study32/pico.kbd.log 2>&1
echo kbd done
