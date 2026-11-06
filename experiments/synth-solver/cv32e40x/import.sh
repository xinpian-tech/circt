#!/usr/bin/env bash
# Import one CV32E40X module (and the SVA bound to it) with circt-verilog.
#   import.sh TOP OUT.mlir [SVA_MODULE...]
# e.g. import.sh cv32e40x_ex_stage ex.mlir ex_stage_env mult div
# (NAME_env binds sv/cv32e40x_NAME_env_sva.sv to cv32e40x_NAME)
# SVA files are bound with sv/binds.sv (bind by module name, .* ports);
# `uvm_error is defined as $error and uvm_pkg is an empty package.
set -eu
H=$(cd "$(dirname "$0")" && pwd)
source $H/../env.sh
CV=${CV:-/var/storage/workspaces/clo91eaf/p530jg-source-20261021/cv32e40x}
top=$1 out=$(realpath -m "$2"); shift 2
R=$CV/rtl
files="$R/include/cv32e40x_pkg.sv $(grep -oE 'DESIGN_RTL_DIR\}/cv32e40x_[a-z0-9_]+\.sv' $CV/cv32e40x_manifest.flist | sed "s|.*}/|$R/|") $CV/bhv/cv32e40x_sim_clock_gate.sv"
sva="" tops="--top=$top"
if [ $# -gt 0 ]; then
  for m in "$@"; do  # local environment files (sv/) first, then the core's sva/
    f=$H/sv/cv32e40x_${m}_sva.sv; [ -f $f ] || f=$CV/sva/cv32e40x_${m}_sva.sv
    sva="$sva $f"
  done
  b=$(mktemp --suffix=.sv)
  { echo "module sva_binds;"
    for m in "$@"; do echo "  bind cv32e40x_${m%_env} cv32e40x_${m}_sva ${m}_sva (.*);"; done
    echo "endmodule"; } > $b
  sva="$H/sv/uvm_stub.sv $sva $b" tops="$tops --top=sva_binds"
fi
# run from / so the source locations resolve from anywhere
cd / && circt-verilog --mlir-print-debuginfo -I $R/include -I $CV/bhv/include \
  '-Duvm_error(a,b)=$error(b);' $files $sva $tops -o $out
