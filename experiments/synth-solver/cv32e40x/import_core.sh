#!/usr/bin/env bash
# Import the whole CV32E40X core with all its SVA bound (bhv/cv32e40x_wrapper.sv,
# including core_log and rvfi) under sv/cv32e40x_core_top.sv. UVM reporting in
# the SVA uses the stand-ins of circt-verilog --uvm-stubs.
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
  sed "s|\${DESIGN_RTL_DIR}|$R|; s|.*/bhv/cv32e40x_wrapper.sv|$wrap|")
# FORMAL leaves out covergroup instances; the write_buffer SVA declares
# `state` as logic for an enum port.
cd / && circt-verilog -DFORMAL -Wno-implicit-port-type-mismatch --uvm-stubs \
  --mlir-print-debuginfo -I $R/include -I $CV/bhv -I $CV/bhv/include \
  -I $CV/sva $files \
  $H/sv/cv32e40x_core_top.sv --top=cv32e40x_core_top ${GFLAGS:-} -o $out
