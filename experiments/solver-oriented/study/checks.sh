# Inputs and check lists of the two designs (sourced).
O=/root/yjh-workspace/solver-oriented-out
CVIN=/var/storage/workspaces/clo91eaf/yjh-workspace/synth-solver-out/core-env
PIN=$O/picorv32/none
CVA=$(ls $O/core-env-final/*.sc.aig | xargs -n1 basename | sed 's/\.sc\.aig$//' | tr '\n' ' ')
PA=$(ls $O/picorv32/models | sed 's/\..*//' | sort -u | grep "^insn_\|^csrw_" | tr '\n' ' ')
