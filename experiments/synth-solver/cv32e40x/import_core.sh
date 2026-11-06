#!/usr/bin/env bash
# Import the whole CV32E40X core with all its SVA bound (bhv/cv32e40x_wrapper.sv
# without core_log/rvfi, see make_wrapper.py) under sv/cv32e40x_core_top.sv.
#   import_core.sh OUT.mlir [DROP]   DROP: comma-separated modules whose SVA
#                                    is left out
set -eu
H=$(cd "$(dirname "$0")" && pwd)
source $H/../env.sh
CV=${CV:-/var/storage/workspaces/clo91eaf/p530jg-source-20261021/cv32e40x}
out=$(realpath -m "$1"); drop=${2:-}
R=$CV/rtl
wrap=$(mktemp --suffix=.sv)
python3 $H/make_wrapper.py $CV $wrap ${drop:+--drop $drop}
files=$(grep -vE '^\s*(//|\+incdir|$)' $CV/cv32e40x_manifest.flist |
  sed "s|\${DESIGN_RTL_DIR}|$R|; s|.*/bhv/cv32e40x_wrapper.sv|$wrap|;
       s|.*/bhv/cv32e40x_sim_clock_gate.sv|$H/sv/cv32e40x_clock_gate_formal.sv|")
# FORMAL leaves out covergroup instances; the write_buffer SVA declares
# `state` as logic for an enum port.
cd / && circt-verilog -DFORMAL -Wno-implicit-port-type-mismatch \
  --mlir-print-debuginfo -I $R/include -I $CV/bhv -I $CV/bhv/include \
  -I $CV/sva '-Duvm_error(a,b)=$error(b);' $H/sv/uvm_stub.sv $files \
  $H/sv/cv32e40x_core_top.sv --top=cv32e40x_core_top -o $out
