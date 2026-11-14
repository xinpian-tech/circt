# A: simplification for solvers

Branch `exp/solver-oriented-simplification`, started 2026-11-14 from
`exp/synth-for-solver` (300af2c3d). Status: plan under discussion, nothing
measured on this branch yet.

## Idea

The synthesis passes of CIRCT (circt-synth) optimize area and delay. A model
checker needs something else: a model that is solved quickly. This branch
designs and implements simplifications in CIRCT whose objective is solver
time, and measures them on several solver back ends.

This is a different idea from branch `exp/compile-once-simplify-many` (B),
which is about when to redo simplifications, not about which ones to do.

## Evidence so far (from exp/synth-for-solver, experiments/synth-solver)

For the idea:

- SOP balancing and the circt-synth `opt` flow make BMC slower (1.18x,
  1.39x) on 97 CV32E40X assertions; they add 8-19% AND gates.
- ConvertCombToDatapath makes one assertion 50x slower for IC3 with fewer
  AND gates.
- `verif-simplify-odc` proves 20 assertions outright from the SVA structure.

Against, or open:

- The largest gain (signal correspondence, BMC 2.8x faster) is a
  reimplementation of ABC `scorr` and equals it; the pass is about 6x
  slower than ABC (316 s against 51 s per model).
- IC3 with rIC3's own preprocessing is insensitive to most combinational
  rewrites.
- There is no cost function yet that says whether a rewrite helps a solver;
  AND count is not one.
- The synthetic miter experiments (word-level rewrites decide whether a
  miter is solved) were removed from the branch on 2026-11-14 (300af2c3d);
  they are in the history.

## Benchmark plan (2026-11-14)

Three model checkers, each without and with preprocessing. The question in
each cell is the solve time of the baseline model against the simplified
model.

| | without preprocessing | with preprocessing |
|---|---|---|
| rBMC | as it is (Tseitin only) | none of its own; candidate: the model after rIC3's preprocessing |
| rIC3 | `ic3 --preproc false`, scorr and frts off | `bmc` and `ic3` with the defaults |
| ABC | `bmc3`, `pdr` alone | `scorr`, `fraig`, `dc2` first (as `dprove` does) |

- Without preprocessing shows what a simplification does by itself.
- With preprocessing shows whether our simplifications are already covered
  by the existing ones, and whether they are better.

Notes:

- ABC has two roles, back end and simplifier to compare with (`scorr`);
  they are reported apart.
- ABC reads latches without an initial value as 0. Models with unknown
  initial values (CV32E40X) need them made explicit (a free input used in
  the first cycle) before any back end sees them, the same model for all.
- ABC `bmc3` propagates constants and hashes while unrolling; its column
  without preprocessing is less pure than rBMC's.
- CaDiCaL with and without inprocessing is a secondary dimension of rBMC
  (do gains only show with a weak SAT solver?), not a back end.
- Models: CV32E40X (97 assertions, with the environment constraints) and
  HWMCC, so that results are not specific to one design.
- Metrics: time to depth 20 or to the counterexample (BMC), solved and time
  (IC3/PDR), with repetitions; latches and ANDs only as a size note.

## First target: signal correspondence (scorr)

What exists:

- ABC `scorr`: Mishchenko, Case, Brayton, Jang, "Scalable and
  scalably-verifiable sequential synthesis", ICCAD 2008. Induction over
  equivalence classes of registers and internal nodes, with speculative
  reduction (Mony, Baumgartner, Mishchenko, Brayton, "Speculative
  reduction-based scalable redundancy identification", DATE 2009). Origin:
  van Eijk, "Sequential equivalence checking based on structural
  similarities", TCAD 2000; with SAT: Bjesse and Claessen, "SAT-based
  verification without state space traversal", FMCAD 2000.
- rIC3 (`src/transys/scorr.rs`, on by default, `--scorr`): latches only,
  and each pair is checked by itself (x = y implies x' = y', 10 restarts),
  not by induction over all classes together. It is weaker than ABC's.
- CIRCT, this branch: `synth-register-correspondence` (registers; with
  `internal` also AND gates; options `constraints`, `property-directed`).
  Same registers merged as ABC on CV32E40X, 12% more ANDs left, slower.

Questions for this target:

- Can CIRCT do better than ABC, using what an AIG does not have: word-level
  registers and operators (classes of words instead of bits), the module
  hierarchy (instances of the same module as candidates), the assumptions,
  the property?
- Can every model use it? AIGER models can go through ImportAIGER and
  ExportAIGER (initial values, bad states and constraints are supported).
  Open: registers with unknown initial values, resets and several clocks
  are treated as free inputs; models of 200k ANDs take over 10 minutes.
- Which back ends gain: BMC gains 2.8x on CV32E40X; rIC3 with its
  preprocessing does its own (weaker) scorr; IC3 without preprocessing is
  not measured yet.

## Results so far (2026-11-14)

Raw data is in `results/`; all times are from a shared machine and noisy.

**scorr on HWMCC25** (`results/hwmcc25-scorr-parts.tsv`, `scorr_parts.sh`,
307 bit-level safety models under 4 MB, 238 complete, 65 time out in
`scorr` at 100 s). Total AND reduction: scleanup 27%, +fraig 38%, lcorr
40%, scorr 53%. The share that needs induction is concentrated: median
1.4% of ANDs per model; 51 models gain more than 20%, almost all miters.

**sc across backends** (`results/core-env-matrix.tsv`, `run_matrix.sh`, 97
CV32E40X properties with environment constraints, depth 20, 150 s). No
model becomes constant. Geometric mean sc/base time on properties both
solve: rBMC 0.67, rIC3 bmc 0.85, ABC bmc3 0.65, rIC3 ic3 0.46 without and
0.58 with its preprocessing, ABC pdr 0.50. ABC's own `scorr; fraig; dc2`
before solving costs more than it saves at this size.

**Register-file cut** (`results/core-env-rfcut.tsv`): removing a third of
the latches gives at most 14% on BMC and no trend on IC3. Size is not
difficulty.

**Hard set.** 19 properties are solved by no backend with base or sc; 11
are checks of the multiplier (`cv32e40x_mult_sva.sv`): the property has
its own `*`, the design has one shared multiplier behind muxes.

**Operator unsharing** (`unshare.py`, a prototype on MLIR text, not yet a
pass): Shannon-expand each `comb.mul` over the muxes on its operands so
every multiplier has mux-free operands and merges with the property's.
Exact; the model grows about 13%. `results/mult-hard-unshare.tsv`
(300 s): `a_mul_result` and `a_shift_result_{ll,lh,hl,hh}` go from
timeout on every backend to solved on every backend tried. Still
unsolved: the three `a_mulh*_result` (64-bit product against a four-cycle
16x16 accumulation) and three `a_shift_result_*` that relate products
across cycles through the accumulator register.

Open: a principled criterion for which muxes to expand; width narrowing
of the expanded multipliers; effect on the properties already solved.
