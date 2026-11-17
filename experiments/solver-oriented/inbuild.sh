#!/usr/bin/env bash
# Run a command in a namespace where this worktree (with its build/
# directory) and the LLVM checkout sit at the paths the build directory was
# configured with, so ninja rebuilds CIRCT only.
#   inbuild.sh CMD...
# e.g. nix-shell experiments/synth-solver/toolchain.nix --run \
#        "experiments/solver-oriented/inbuild.sh ninja -C build bin/circt-opt"
# The build directory is a copy of the one configured at
# $P/circt (now $P/circt.moved), kept in build/ of this worktree.
W=$(cd "$(dirname "$0")/../.." && pwd)
P=${CONFIGURED_PARENT:-/var/storage/workspaces/clo91eaf}
M=${LLVM_CHECKOUT:-$P/circt.moved}
# An empty local directory stands in for $P inside the namespace, so that
# $P/circt can be a mount point without creating it on the shared storage.
F=${BWRAP_PARENT:-$(dirname "$W")/.bwrap-parent}
mkdir -p "$F" "$W/.venv" "$W/build"
exec bwrap --bind / / --dev-bind /dev /dev --proc /proc \
  --bind "$F" "$P" \
  --bind "$P/yjh-workspace" "$P/yjh-workspace" \
  --bind "$M" "$P/circt.moved" \
  --bind "$W" "$P/circt" \
  --bind "$M/llvm" "$P/circt/llvm" \
  --bind "$M/.venv" "$P/circt/.venv" \
  --chdir "$P/circt" "$@"
