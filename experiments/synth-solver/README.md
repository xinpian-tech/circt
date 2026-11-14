# CIRCT simplifications for model checkers

Experiments on which simplifications a hardware compiler should apply so
that model checkers solve faster. The CIRCT passes themselves are on the
branch `review/synth-for-solver`; this directory holds the scripts, notes
and results.

- `REVIEW.md`: the simplification passes by layer and their commits on the
  review branch.
- `WORKFLOW.md`: how the experiments are run and how a pass is added.
- `PROGRESS.md`: what was done and what is open.
- `cv32e40x/`: CV32E40X with its SVA through CIRCT to AIGER, results with
  rIC3 and rBMC (README there).
- `hwmcc/`: checks of the timing-changing passes on HWMCC models.
- `upstream-issues/`: ImportVerilog issues with minimal reproducers.
- `btor2mlir.py`, `fuzz_remove_false_cycles.py`, `env.sh`, `toolchain.nix`:
  BTOR2 importer (unused), fuzzer of synth-remove-false-cycles, environment.
