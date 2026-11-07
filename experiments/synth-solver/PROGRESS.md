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

1. Synthetic miters of the CombRewriter (ASP-DAC 2026) rewrite patterns
   (`gen.py`, `run.sh`, README runs 1-2):
   - CIRCT word-level canonicalize decides commutativity/reordering miters
     (raw TO -> 0.05 s); bit-level passes (dc2, functional reduction) cannot
     recover them.
   - Canonicalize's n-ary flattening hurts the shared-product pattern (raw
     solves, canonicalized TO).
   - Prototype solver-oriented canonicalizer `solvercanon.py` (mux
     push-down, associative chains that keep shared sub-terms): 112/112
     solved vs 94/112 for circt-synth; mux push-down and associativity are
     the rules that matter; result is larger than area-optimal.
   - Caveat: models are built from the targeted patterns.
2. Real design, rocket-chip TinyConfig (`rocket/`, README there):
   - Chisel -> firtool --lower-to-core -> `prep.py` (CIRCT Python bindings:
     flatten, reset, memories, asserts -> bad, COI) -> configs -> AIGER.
   - 40 random asserts, both firtool -O levels, 5 configs, 3 solvers: all
     UNSAT and consistent; simplification cuts ANDs up to 47% but changes
     solved counts by <= 3/40 with no consistent direction. Structural
     hashing is the only necessary step (26.5M ANDs without it).
   - TileLink protocol properties have no mul/mux datapath, so the word-level
     rewrites of item 1 have nothing to act on there.
3. `btor2mlir.py`: BTOR2 (bit-vector) importer, written for HWMCC'24 BV
   (downloaded to yjh-workspace/hwmcc24-bv), not used after switching to
   rocket-chip.

4. CV32E40X (`cv32e40x/`, README there), 2026-11-06:
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

   - Whole core with all SVA (except debug_triggers, mpu): import and LLHD
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

## Open next steps (not started)

- Environment assumptions for the core (OBI protocol, interrupts, debug) so
  that SAT results mean design bugs.
- Replace lower_regs.py by C++ passes (in progress: assertion labels,
  verif-assertions-to-output, seq-lower-firreg-to-compreg, ExportAIGER
  initial values, llhd-combine-drives for piecewise signals).

- The 3 rocket asserts never solved and the unstable ones (e.g.
  assert__assert_103#23): find what makes them hard (invariant size vs query
  hardness); test rIC3 `--inn`. If internal signals help, let CIRCT export
  word-level predicates as candidate lemma variables.
- Designs with real datapaths (multipliers, ALU/FPU properties, or
  equivalence of a design and its rewritten version) for the word-level
  rewrites; turn `solvercanon.py` into a CIRCT pass if they help.
- Larger assert sample on rocket; other configs (DefaultConfig).

## Locations

- Code/results: this directory (`results-run*.tsv`, `rocket/results-run2.tsv`).
- Data: /var/storage/workspaces/clo91eaf/yjh-workspace/{microexp-data/synth-solver*, rocket-mlir, rocket-chip, hwmcc24-bv}.
- CIRCT build: ../../build (local, not committed).
