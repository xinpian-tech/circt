# Progress (paused 2026-11-04)

Question: which simplifications should a hardware compiler (CIRCT) apply so
that model checkers solve faster, given that today's AIG optimizations aim at
area and delay? Can the compiler use structure it knows (word-level ops,
hierarchy, meaningful predicates) to do what the solver cannot?

## Background from earlier work (aigopt / rIC3, notes/microexp M5-M11)

- Latch-only IC3/PDR learns the same lemmas whatever the combinational
  structure (function-determined); structure only changes per-query SAT cost.
- rIC3's own preprocessing (BVE, CNF compaction) evens out most bit-level
  AIG rewrites; the only stable gain on HWMCC came from sequential scorr.
- So combinational aig2aig optimization for IC3 is close to a false premise;
  useful directions are sequential/transition-system transforms, word-level
  structure, and giving IC3 better lemma variables (`--inn`).

## This branch (exp/synth-for-solver, rebased on upstream llvm/circt main)

Tools: upstream CIRCT built locally with cmake (`toolchain.nix`, `env.sh`;
the repo's nix flake failed on truncated source downloads); rIC3 and ABC.

The synthetic miter experiments (CombRewriter rewrite patterns, the
`solvercanon.py` prototype) and the rocket-chip TinyConfig experiments were
removed from the branch on 2026-11-14; they are in the history before that
date.

1. `btor2mlir.py`: BTOR2 (bit-vector) importer, written for HWMCC'24 BV
   (downloaded to yjh-workspace/hwmcc24-bv), not used.

2. CV32E40X (`cv32e40x/`, README there), 2026-11-06:
   - SystemVerilog + SVA -> circt-verilog -> `lower_regs.py` (registers with
     reset semantics, LLHD signals, assertions -> bad) -> AIGER; 24 modules
     export. ex_stage with the mult/div SVA: 12 multiplier checks time out
     under every synthesis configuration.
   - circt-synth's ConvertCombToDatapath makes a_check_acc_init_zero 50x
     slower for IC3 (1-2 s -> 40-117 s) with fewer ANDs.
   - New pass `verif-simplify-odc`: simplify assertion logic with its
     observability don't-cares (facts from the enable, the antecedent, and
     mux selects / and-or operands inside the property), with restricted
     canonicalization, hashing that ignores names, case splits. The
     antecedent alone solves 5 of the 12 (one folds away, four in < 1 s,
     a_mul_result in 60-146 s); the rest need sequential invariants on the
     MULH accumulator. The nested don't-cares change nothing on ex_stage;
     properties with more structure are needed (core-level SVA).

   - Whole core with all SVA (debug_triggers and mpu left out until
     2026-11-07, imported since, 17 more assertions): import and LLHD
     fixed in CIRCT, 266 assertions export. The pass proves 14 and turns 4
     timeouts into 3-5 s proofs; other proof times barely change. One
     cycle of bits (wb_stage <-> controller_fsm) is left; run 2 cut it with
     free inputs (only UNSAT valid; option since removed).
   - New pass `synth-remove-false-cycles`: rebuilds cycles bit by bit,
     removes those whose cut bits stop mattering after k iterations (SAT,
     CaDiCaL), and otherwise rebuilds them as their unique fixed point under
     an obligation output that the model checker proves 0. The core's cycle
     needs the obligation (only well defined in reachable states); proved
     for all models. Run 3 (exact): antecedent-only solves 6 more (the
     multiplier checks a_mul_result, a_shift_result_*), loses none.
   - lower_regs.py replaced by C++ passes (lower.sh): assertion labels in
     ImportVerilog, verif-assertions-to-output, seq-lower-firreg-to-compreg
     (exact async reset), ExportAIGER latch initial values,
     llhd-combine-drives for piecewise and read-modify-write signals. Run 4
     agrees with run 3 on every assertion.

## Open next steps (not started)

- Environment assumptions for the core (OBI protocol, interrupts, debug) so
  that SAT results mean design bugs.
- Sequence operators not yet lowered by verif-assertions-to-output
  (`until`, goto repetition, sequence `or`; 7 core assertions).

- Designs with real datapaths (multipliers, ALU/FPU properties, or
  equivalence of a design and its rewritten version) for word-level
  rewrites.

## Locations

- Code/results: this directory (`cv32e40x/results-*.tsv`).
- Data: /var/storage/workspaces/clo91eaf/yjh-workspace/{microexp-data/synth-solver*, hwmcc24-bv}.
- CIRCT build: ../../build (local, not committed).
