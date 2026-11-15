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

**Unsharing everywhere costs** (`results/core-env-unshare-all.tsv`, all 97
properties, 150 s): the five multiplier properties are gained on every
backend, but properties that do not use the multiplier get a model 19%
larger; rBMC loses 11 that were near the time limit.

**Criterion** (`ushit.py`): expand a multiplier only if one of the
resulting multipliers has the operands of a multiplier already in the
model (also when wider, with extended operands). On the 97 properties it
fires on 8, all checks of the multiplier, and leaves 89 models unchanged
(`results/core-env-unshare-hits.tsv`). The simplification therefore
depends on the property.

**tc + fraig against sc in solve time**
(`results/core-func-tcfraig-matrix.tsv`, models without environment
constraints, properties solved in all four configurations and not
constant after sc). Geometric mean against base: rBMC tc 0.94, tc+fraig
0.78, sc 0.64 (66 properties); rIC3 ic3 without preprocessing tc 0.41,
tc+fraig 0.45, sc 0.34 (35 properties). The numbers of solved properties
barely change: these speed up what is solvable; unsharing changes what
is solvable.

**The pass** (`comb-unshare-operators`,
`lib/Dialect/Comb/Transforms/UnshareOperators.cpp`, test
`test/Dialect/Comb/unshare-operators.mlir`). Unlike the prototype it does
not copy multipliers: it compares operands bit by bit (through concat,
extract, replicate and the muxes resolved by an assignment of selects),
finds the assignments under which the operands of a shared multiplier are
those of another multiplier, and takes the (low bits of the) result from
that one under their disjunction. Run after `canonicalize,cse`, before
`hw-cone-of-influence`. On the 97 properties it rewrites 9
(`results/core-env-unshare-pass-hits.txt`) and the models grow by 0.2%.
`results/mult-hard-unshare-pass.tsv` (300 s): the same five properties are
solved on all four backends, faster than with the prototype
(`a_mul_result`: rBMC 42 s, ABC bmc3 23 s, rIC3 ic3 10 s).

Build note: `inbuild.sh` runs a command in a namespace that maps this
worktree, the LLVM checkout and a copy of the old build directory to the
paths the build was configured with (edit the paths at its top), so only
CIRCT is rebuilt.

**Cross-cycle multiplier checks: uninterpreted-function abstraction**
(`ufmul.py`, an experiment on MLIR text; `results/mult-hard-uf.tsv`,
300 s). Each multiplier result becomes a free input, with assumptions
for functional consistency within a cycle and against the operands and
result of the previous cycle. An over-approximation. Outcome:
`a_check_result_constant` reaches depth 20 in BMC (70-90 s; frame 7
before), IC3 still times out; the three accumulator checks
(`a_shift_result_ll_shift`, `_ll_lh`, `_ll_lh_hl_shift`) and the
full-product checks get spurious counterexamples. The accumulator checks
rely on arithmetic facts the abstraction drops (the product of two
zero-extended 16-bit values has zero top bits, so an arithmetic and a
logical shift agree) and on values two or more cycles back. Making this
work needs refinement (CEGAR) or partially interpreted multipliers; not
pursued yet.

With two additions to `ufmul.py` the spurious counterexamples on the
accumulator checks are gone (`results/mult-hard-uf-lemmas.tsv`): range
lemmas (operands zero- or sign-extended from ka and kb bits give a
product that fits in ka+kb bits), and consistency between multipliers of
different widths on their low bits. A one-step history is enough: the
property's multipliers are evaluated every cycle, so with stable
operands they carry the result forward and meet the value the register
took from the shared multiplier. `a_shift_result_ll_shift` is proved by
IC3 (14-33 s) and reaches depth 20 in BMC; `_ll_lh`, `_ll_lh_hl_shift`
and `a_check_result_constant` reach depth 20 in BMC (55-160 s, stuck at
frames 6-7 before) but IC3 still times out at 300 s.

`comb-unshare-operators` now also handles two-operand divisions, modulos
and shifts (same width and operand order only). On the 97 CV32E40X
properties the hits are unchanged (the same 9, all multipliers): the
divider is iterative and no property restates a shift.

**The abstraction as a pass** (`verif-abstract-operators`,
`lib/Dialect/Verif/Transforms/AbstractOperators.cpp`, test
`test/Dialect/Verif/abstract-operators.mlir`): what `ufmul.py` does, with
the candidate widths of the range assumptions taken from the operand
cones instead of a fixed list. It abstracts every multiplier of the
module, so it is only sound to trust its proofs (and bounded proofs);
counterexamples need to be replayed on the original model, which is not
automated yet. `results/mult-hard-abstract-pass.tsv` (300 s) matches the
script: all four cross-cycle checks reach depth 20 in BMC (39-177 s),
`a_shift_result_ll_shift` is proved by IC3 (19-34 s).

**Abstraction, last state** (`results/mult-hard-abstract-bounds.tsv`,
`abstract_check.sh`, `replay.py`, `splitmul.py`). `observe` adds outputs
with the operands and free result of each multiplier; `replay.py`
compares them with the real product along a counterexample. On the three
`a_mulh*_result` checks, with the property's 64-bit multiplier split at
bit 16 into four partial products (`splitmul.py`, exact), the first
counterexample was spurious because a free partial product exceeded
(2^16 - 1)^2, which the accumulation relies on; the range assumptions
are now these tight bounds, and all three reach depth 20 in rBMC
(225-278 s; frame 7 before). IC3 does not prove them in 300 s.

The abstraction changes behaviour (it is an over-approximation), which is
not the kind of simplification this branch is about; it stays here as a
side experiment and is not developed further. The line of work is the
exact rewrite, `comb-unshare-operators`.

## Evaluation of comb-unshare-operators with sc (2026-11-15)

`export_cfgs.sh` builds base, us (unsharing), sc and ussc (unsharing, then
sc) for the 97 properties with one binary; `results/eval-export.log` has
the sizes. For 88 properties the us model is byte-identical to base
(checked with `cmp`), so only base and sc were solved for them
(`results/eval-same.tsv`); the 9 changed ones were solved in all four
configurations (`results/eval-changed.tsv`). 150 s, depth 20.

Solved of 97:

| backend | base | us | sc | us+sc |
|---|---|---|---|---|
| rBMC | 72 | 77 | 75 | 79 |
| ABC bmc3 | 77 | 82 | 77 | 80 |
| rIC3 ic3, no preprocessing | 60 | 65 | 63 | 68 |
| rIC3 ic3 | 67 | 72 | 65 | 70 |

Unsharing adds the same five properties on every backend; sc alone adds
none of them. sc after unsharing is not uniformly better on those five:
`a_shift_result_ll` and `_lh` drop to 0-4 s, but `a_mul_result`,
`a_shift_result_hh` and `_hl` get slower (11-38 s to 23-147 s) and three
backend/property pairs time out. Why sc undoes part of the benefit there
is not understood yet.

Which part of sc does it (`results/eval-sc-parts-after-unshare.tsv`, the
three properties that got slower, 150 s): after unsharing, register
correspondence alone (usrc), ternary constants (ustc), ternary constants
plus functional reduction (ustcfr) and functional reduction alone (usfr)
all keep the properties solved in 4-69 s on rBMC, ABC bmc3 and rIC3 ic3.
Only the inductive merging of internal nodes
(`synth-register-correspondence{internal}`) makes them slower. Until that
is understood, follow unsharing with `tc + fr` or `rc`, not with sc.
