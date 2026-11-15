#!/usr/bin/env bash
# run a command inside a namespace where the CIRCT worktree, the LLVM
# checkout and the copied build directory sit at the paths the build was
# configured with. usage: inbuild.sh CMD...
exec bwrap --bind / / --dev-bind /dev /dev --proc /proc \
  --bind /tmp/claude-0/-root-yjh-workspace/3cca2a40-f56d-411e-8bea-05778e44e84e/scratchpad/fakeparent /var/storage/workspaces/clo91eaf \
  --bind /var/storage/workspaces/clo91eaf/yjh-workspace /var/storage/workspaces/clo91eaf/yjh-workspace \
  --bind /var/storage/workspaces/clo91eaf/circt.moved /var/storage/workspaces/clo91eaf/circt.moved \
  --bind /tmp/claude-0/-root-yjh-workspace/3cca2a40-f56d-411e-8bea-05778e44e84e/scratchpad/exp-wt /var/storage/workspaces/clo91eaf/circt \
  --bind /var/storage/workspaces/clo91eaf/circt.moved/llvm /var/storage/workspaces/clo91eaf/circt/llvm \
  --bind /var/storage/workspaces/clo91eaf/circt.moved/.venv /var/storage/workspaces/clo91eaf/circt/.venv \
  --bind /tmp/claude-0/-root-yjh-workspace/3cca2a40-f56d-411e-8bea-05778e44e84e/scratchpad/build /var/storage/workspaces/clo91eaf/circt/build \
  --chdir /var/storage/workspaces/clo91eaf/circt "$@"
