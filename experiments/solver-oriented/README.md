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

Build note: the worktree is `/root/yjh-workspace/circt-solver-oriented`
(local disk), with the build directory in its `build/` (a copy of the one
configured at `/var/storage/workspaces/clo91eaf/circt`, now
`circt.moved`). `inbuild.sh` runs a command in a namespace that maps the
worktree and the LLVM checkout of `circt.moved` to the configured paths,
so only CIRCT is rebuilt:

    nix-shell experiments/synth-solver/toolchain.nix --run \
      "experiments/solver-oriented/inbuild.sh ninja -C build bin/circt-opt"

The tools run outside the namespace with
`LD_LIBRARY_PATH=/nix/store/41ym1jm1b7j3rhglk82gwg9jml26z1km-gcc-14.3.0-lib/lib`.

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

**Why sc slows the unshared properties, and operand isolation**
(`results/sc-after-unshare-seeds.tsv`, `-isolate-seeds.tsv`: property,
config, solver, seed, solved, seconds; 200 s). The slowdown is stable
over solver seeds (2.3-3x in rBMC, 2.5-4x in rIC3 ic3). Hypothesis: the
rewritten result `cond ? other : shared` equals `shared` in every case,
so an equivalence-merging pass can prove bits of it equal to the shared
operator's and merge them back, undoing the rewrite for those bits.

The pass now isolates the shared operator (`isolate`, on by default):
where its whole result is taken from another operator its operands
become constants, so the two are no longer equal everywhere. Still
exact. Medians in seconds, rBMC / rIC3 ic3:

| property | us before | us+sc before | us isolated | us+sc isolated |
|---|---|---|---|---|
| a_shift_result_hh | 23 / 4 | 65 / 11 | 24 / 4 | 16 / 4 |
| a_shift_result_hl | 35 / 4 | 109 / 11 | 18 / 4 | 40 / 5 |
| a_mul_result (not isolated) | 38 / 7 | 86 / 33 | 37 / 8 | 87 / 39 |

`a_mul_result` is the control: its hit is on the low 32 of 34 bits, the
high bits of the shared multiplier are still read, so it cannot be
isolated, and its slowdown stays. `hh` is fixed, `hl` mostly (rBMC still
2x). This supports the hypothesis but the merges themselves were not
inspected.

**Widening** (`widen`, on by default; `results/sc-after-unshare-widen-seeds.tsv`).
When the other multiplier is narrower, the high bits now come from a copy
of the shared multiplier with the operands it has under the assignment
(mux-free), so the whole result is replaced and isolation applies. One
more multiplier per such match (`a_mul_result`: 60150 to 60818 ANDs).
`a_mul_result`, medians in seconds rBMC / rIC3 ic3: us 39 / 7, us+sc
27 / 8.5 (before: 87 / 39); one of three rIC3 seeds on us+sc timed out
at 200 s. All five solved properties are now at least as fast with sc as
without, except `a_shift_result_hl` in rBMC (18 to 40 s), which is
unexplained.

**Evaluation with isolation and widening** (`results/eval-changed-widen.tsv`:
us and us+sc for the 9 rewritten properties with the final pass; base and
sc and the other 88 properties as in the table above). Solved of 97:

| backend | base | us | sc | us+sc |
|---|---|---|---|---|
| rBMC | 72 | 77 | 75 | 80 |
| ABC bmc3 | 77 | 82 | 77 | 82 |
| rIC3 ic3, no preprocessing | 60 | 64 | 63 | 68 |
| rIC3 ic3 | 67 | 72 | 65 | 70 |

us+sc now solves the five multiplier properties on all four backends
(three backend/property pairs timed out before). One regression to
note: `a_mul_result` under us alone times out on rIC3 ic3 without
preprocessing (14 s before widening); rIC3 was seed-sensitive on this
model in the seed runs, so this needs repeated runs before blaming the
widening.

## Why sc slowed a_shift_result_hl after unsharing (2026-11-15)

Material in `results/hl-sc-mechanism/`. Debugging options were added to
`synth-register-correspondence`: `merge-from`/`merge-to` merge only a
range of the proved classes, `merge-dump` prints each merged gate and its
representative.

- The earlier one-off timeout of `a_mul_result` (us, rIC3 without
  preprocessing) is rIC3's default seed: eight other seeds give the same
  median with and without widening (`results/a_mul_result-us-ric3-seeds.tsv`).
- Merging single ranges of classes gave unstable answers: on this
  instance one gate merge can move one solver between about 12 and 18 s,
  a different merge for each solver (`perturbation-distribution.tsv`).
  Merging prefixes of the classes is stable (`timings.txt`): three
  engines step from 11-12 s to 20-26 s exactly when class 2644 is added.
- Class 2644 is one gate, `p & !x`, replaced by `q & !x`, where p and q
  read only the operand register through about 2600 ops (bits derived
  from the product) and x is a control cone (`class-2644.txt`). It sits
  in the property's comparator. After unsharing the comparator compares
  `cond ? e : m` with `e` bit by bit, the same node on both sides when
  cond holds; sc replaced one term by an equivalent one built from
  another bit of the product (equal in every reachable state), so that
  bit of the comparison became a fact about the multiplier that the
  solver has to prove.
- Reproduction at word level: replacing bit 33 of the reference product by
  its bit 32 in the comparison (they are always equal) takes the three
  engines from 11-18 s to a timeout at 300 s. Stronger than what sc did
  (it replaced one term, not the whole bit), same direction.

Fix in the pass (`fold-compare`, on by default): a comparison of the
shared operator with the operator it now takes its result from,
`eq(cond ? e : m, e)`, becomes `cond | eq(m, e)`. Under cond the
comparison holds by construction and no later rewrite of the bits of `e`
can turn it into a statement about the operator. Exact. It applies to the
four `a_shift_result_*` properties (the comparison is direct), not to
`a_mul_result` (an addition and a truncation are in between). The four
get faster by themselves (rBMC 11-24 s to 1-4 s) and stay fast with sc;
see `timings.txt`.

## Full table on the final passes (2026-11-17)

All 97 properties re-exported with the final binary; `export_cfgs.sh` and
`export_parts.sh`, then `run_matrix.sh`, K=20, 150 s. Unsharing changes 9
properties; the other 88 reuse base for us, sc for ussc and so on. Data in
`results/final/`. "Solved" means different things per back end: K (no
counterexample up to 20 frames) for rBMC and ABC bmc3, a proof (UNSAT) for
rIC3 ic3. No property has a counterexample.

Solved (without / with unsharing), and geometric mean seconds on the
properties all ten configurations solve:

| config | rBMC | ABC bmc3 | rIC3 ic3, no front end | rIC3 ic3 |
|---|---|---|---|---|
| base  | 73/78, 33.8 | 76/81, 10.5 | 59/63, 5.54 | 66/71, 8.04 |
| tc    | 73/78, 33.1 | 77/82, 10.9 | 54/59, 3.20 | 66/71, 6.82 |
| fr    | 73/78, 29.9 | 76/81, 9.1  | 61/66, 4.39 | 62/67, 7.55 |
| tc+fr | 74/79, 29.5 | 77/82, 8.4  | 60/65, 3.26 | 61/66, 7.09 |
| sc    | 75/80, 22.1 | 77/82, 6.5  | 62/67, 2.31 | 65/70, 5.42 |

- Unsharing adds 5 on every configuration and back end.
- sc is the fastest everywhere; tc+fr gets part of it (rBMC 13% vs 35%).
- The rIC3 solved counts move by up to 5 between configurations in both
  directions; properties near the 150 s limit flip, so differences of a few
  properties are noise (see the seeds runs below).
- ABC with its own preprocessing (`scorr; fraig; dc2`, time included) solves
  57 / 61 / 64 on base / sc / ussc against 76 / 77 / 82 without it, and no
  property only with it. Its run time drops from 98 s to 54 s on sc models.

On the sc models rIC3's own scorr and frts merge nothing on all 97
properties (on base models they always do, up to 207 latches): CIRCT's sc
covers them.

## rIC3 front end on the final models (2026-11-17)

`ric3_frontend.sh` on the ussc models: each front-end item alone. Middle
set: 29 properties where on or off needed more than 10 s or timed out (3
seeds, 300 s, solved = solved by most seeds, time = geometric mean of the
median over the 11 properties every variant solves in every seed):

| variant | solved | time |
|---|---|---|
| on         | 29 | 10.5 |
| off        | 27 | 20.0 |
| only-bve   | 29 | 7.0  |
| only-cnf   | 29 | 8.6  |
| only-scorr | 28 | 30.7 |
| only-simp  | 27 | 17.0 |
| only-frts  | 24 | 20.8 |

Easy set (44, 60 s, one seed): all solve; on 4.1 s, off 1.0 s, only-scorr
8.9 s, only-frts 3.3 s, only-cnf 0.66 s, only-bve 0.77 s. Hard set (24 both
timed out at 150 s; on and off, 2 seeds, 300 s): off none, on one
(a_single_step_no_irq_id, 96 s and 299.6 s).

The circuit-level items (scorr, frts, fe-simp) bring nothing after CIRCT;
the remaining gap between on and off is in the CNF encoding (compact
encoding, bounded variable elimination).

## Latches rIC3 still removed: observability don't cares (2026-11-17)

After sc, rIC3's trivial simplification (cone of influence after bve on the
compact CNF) removed 8 of 2509 latches on every model. With an instrumented
rIC3 (`RIC3_DEBUG_COI=1`, prints the latches its cone of influence drops)
they are `load_store_unit_i/rdata_q[7:0]`:

    rdata_is_split = (size == word && offset != 0) || (size == half && offset == 3)
    rdata_full     = rdata_is_split ? {resp_rdata, rdata_q} : {resp_rdata, resp_rdata}
    rdata_aligned  = rdata_full >> (8 * offset)        // only [31:0] used

rdata_q is selected only when the offset is not zero, so its low byte is
never shifted into the result; its next state is resp_rdata or the result.
Tying the 8 bits to 0 leaves bad, constraints and all other next states
equal (ABC `cec`); each bit alone is observable, only the group is not. No
equivalence-based pass removes them: nothing in the circuit is equal to
what they could be replaced by.

Two passes (commit 083ea67ef): `comb-mux-dont-care` zeroes mux-arm bits no
shift can bring into a used bit while the arm is selected (select and shift
amounts enumerated over the bits they depend on, at most 12), and
`seq-trim-registers`, a bit-level cone of influence that narrows registers
to their observed bits. On a_mul_result after sc: 2509 -> 2501 latches,
45301 -> 45228 ands, and rIC3's trivial simplification, scorr and frts then
remove nothing. `export_cfgs.sh` runs them with DC=1.

## Solver preprocessing after CIRCT: ABC, CaDiCaL, rIC3 encoding (2026-11-17)

ABC, step by step on the plain models (`results/final/abcpre.txt`; 82
properties, 15 more whose output sc already made constant; medians):

| step | base | sc |
|---|---|---|
| scorr | -16.8% latches (~508), -25% ands | -0.04% latches (1-2), -0.3% ands |
| fraig after scorr | 0 | 0 |
| fraig alone | -14.9% ands | 0 |
| dc2 alone | -19.4% ands | -11.8% ands |

ABC's scorr and fraig find nothing after sc (the 1-2 latches are likely the
helper latches aigplain.py adds). dc2, an area-oriented rewriting, still
removes 12% of the ands; whether that helps solving is measured with
`results-dc2.tsv`.

CaDiCaL (rBMC, `rbmc-nopre`: elim, subsume, probe, vivify, ternary and
decompose off; same batch): base 70 with, 52 without (18 only with); sc 73
with, 69 without (4 only with: a_debug_taken_rf_write, a_max_seq_len_pop,
a_max_seq_len_popret, a_sleep_to_irq); equal time on the common ones.

rIC3 on the ussc models, on / off / enc (only compact CNF and bve), same
batch: middle set solved by most seeds 29 / 28 / 28 (runs 85 / 79 / 81 of
87), geometric mean on the 18 common 9.2 / 23.6 / 6.9 s; easy set 3.60 /
1.09 / 0.63 s. With the circuit-level items off rIC3 is faster and loses
one borderline property (a_if_ex_trig_exclusive, 2 of 3 seeds).

The don't-care passes (DC=1) on all 97: solved counts within noise of the
earlier ussc (rBMC 80 -> 79, ABC 82 -> 83, rIC3 bare 67 -> 66, rIC3 70 ->
68, different properties in both directions), same times.

dc2 after sc (`results-dc2.tsv`, scplain vs scdc2, same batch, 150 s):
solved rBMC 74 / 75, ABC bmc3 78 / 77, rIC3 bare 55 / 55, rIC3 61 / 58,
with different properties in both directions (noise). Geometric mean on the
commonly solved ones taking over 5 s: rBMC 56.2 / 47.5 s, ABC 22.7 / 21.7 s,
rIC3 bare 16.5 / 12.4 s, rIC3 15.8 / 12.8 s. dc2 solves nothing new but makes
the hard ones 5-25% faster: AIG rewriting is the one solver-side step CIRCT
does not cover yet.

## PicoRV32 with the riscv-formal checks (2026-11-18)

Second design: PicoRV32 (`COMPRESSED_ISA`, fast mul, div, barrel shifter)
with the riscv-formal checks generated by `genchecks.py`, one safety model
per check (`picorv32/import.sh`, `build_models.sh`; `lower.sh --immediate`).
New in CIRCT for this: immediate assertions through
llhd-remove-control-flow and verif-assertions-to-output,
llhd-sample-process-assertions for assertions in clocked processes, and
llhd-unroll-loops in processes. The free values of riscv-formal (anyseq,
anyconst) become inputs of the top. Checked against SymbiYosys on the
planted bugs (reg fails on TESTBUG_001, pc_fwd on 005, insn_add on 004;
unique, causal on 001 and pc_bwd on 005 pass, as with sby).

82 of 86 checks export (cover left out; the 4 csr_ill fold to constants);
about 1.8-2.3k latches and 31-35k ands, sc removes about 25% of the ands.
comb-unshare-operators changes nothing. Results in `results/picorv32/`
(150 s, K=21 for the insn/csrw checks, K=31 for the other 10):

| back end | insn/csrw (72) base / sc | others (10) base / sc |
|---|---|---|
| rBMC | 68 / 69, geo 58.4 / 47.5 s | 9 / 9, 10.7 / 5.0 s |
| ABC bmc3 | 64 / 64, 39.5 / 36.9 s | 9 / 9, 1.2 / 0.8 s |
| rIC3, rIC3 bare | 0 / 0 | 0 / 0 |

The checks are bounded (checked in one cycle, counted from reset): IC3
proves none. ABC with its preprocessing (scorr, fraig, dc2) on sc: 63
solved (64 without), slower (52.2 vs 36.9 s).

CaDiCaL on the 72 sc models: all on 69, all six off 28-29; only
elimination on 68, only elimination off 28 (geometric mean on the 26
solved by all: 34.0, 39.1, 94.7, 86.7 s). The gap is CaDiCaL's bounded
variable elimination, CNF level and out of scope; its other preprocessing
(subsumption, probing, vivification, ternary resolution, decomposition)
brings nothing after sc.

## Word-level known bits (seq-known-bits) on solving (2026-11-18)

`seq-known-bits` (KnownBits abstract interpretation through registers to a
fixpoint, then constants) before sc, 97 CV32E40X properties, configurations
base, kb (`KB=1 export_cfgs.sh`), sc, kbsc. Results in
`results/core-env-kbcmp/results.tsv`. Solved / geometric mean over the
properties both sides solve (hard: one side >= 10 s):

| backend | base | kb | sc | kbsc | base→kb gm (hard) | sc→kbsc gm (hard) |
|---|---|---|---|---|---|---|
| abc-bmc3 | 77 | 75 | 77 | 78 | 10.77→10.80 (32.0→32.3) | 7.02→7.42 (29.5→31.6) |
| rbmc | 72 | 73 | 73 | 74 | 33.50→32.75 (64.8→64.2) | 22.69→21.60 (48.6→46.0) |
| ric3-ic3-bare | 59 | 58 | 63 | 61 | 6.49→4.45 (27.8→16.7) | 3.31→3.18 (23.5→20.6) |

kb alone does not help BMC (the constant bits are easy for SAT); it speeds
bare IC3 by ~30-40% without changing solved counts (4/3 and 5/3 properties
solved by one side only: IC3 variance). kb+sc solves like sc; kb's value is
the 15-22% cheaper sc after it (fewer candidates). Cheap word-level constant
invariants are not enough on their own; next: word-level equivalences.

## Word-level simplification before sc (2026-11-18)

Where sc's reduction comes from, on a_addr_incr (ABC strash, ANDs/latches):
base 55,854/3,081; kb 51,001; base+fraig 47,459; kb+fraig 43,416; sc
41,154/2,509. About half of it is combinational (fraig, no induction). With
locations on the word operations, the gates fraig merges come mostly (62%)
from reset muxes around logic that already selects the reset value under
reset (async reset lowered twice, on the wide pipeline registers), then from
priority decoders `x == A && !(x == B)`. `comb-context-simplify` (operands
and mux arms evaluated on known bits under the facts of their context) and
`seq-known-bits{context}` (mux arms under their conditions) are the word
level answer (`WS=1 export_cfgs.sh`, 0.4 s): 46,854/3,013, +fraig
42,896, +sc 40,938/2,510. What sc still finds beyond them is mostly constant
bits of pipeline registers (342 bits; relational) and one shadow register
of an assertion. `seq-word-congruence` (register partition x known bits,
no solver) finds almost nothing (21 registers, no size change).

97 properties (`results/core-env-wscmp`, base/kb/sc from core-env-kbcmp):
mean ANDs base 56,719, ws 46,702, sc 41,936, wssc 41,711; sc export time
354 s alone, 194 s after ws (-45%).

| backend | base | sc | ws | wssc | base→ws gm (hard) | sc→wssc gm (hard) |
|---|---|---|---|---|---|---|
| abc-bmc3 | 77 | 77 | 77 | 78 | 11.51→11.37 (31.2→32.7) | 7.02→7.00 (27.9→27.7) |
| rbmc | 72 | 73 | 72 | 74 | 33.50→28.79 (64.8→56.0) | 22.69→22.36 (48.6→47.7) |
| ric3-ic3-bare | 59 | 63 | 57 | 63 | 5.65→3.28 (26.0→11.9) | 3.45→3.39 (16.9→14.1) |

ws alone gives rBMC 14% and bare IC3 ~40% over base but stays behind sc
(the induction part); wssc solves like sc or one more, with sc 45% cheaper.

## ws2: word congruence with case splits, trimmed registers (2026-11-18)

Correction to the section above: the 342 pipeline-register bits sc seemed to
add after ws were already known to `seq-known-bits{context}` (340); the
registers stayed whole, so the exported models kept them as dangling
latches (solvers drop them). With dangling latches removed (ABC `&scl`), on
a_addr_incr: base 50,348/2,616, ws 46,423/2,584, sc 41,154/2,509. The real
sequential gap was 75 latches, mostly register equalities.

`seq-word-congruence` now also compares next states numbered apart with
`equalUnder` (aligned slices through concat/extract, known bits under facts,
unit propagation of disjunctive facts, case splits on mux conditions, no
solver): it merges an assertion's shadow register of the fetch address
(`prefetcher_sva/previous_addr` = `trans_addr_q`). `seq-trim-registers`
drops the known and unread register bits. ws2 = ws + both (`WS=1`):
46,181/2,552 (+fraig 42,135); 43,974 with ripple-carry adders
(`ADDER=RIPPLE-CARRY`, `convert-comb-to-synth{adder-architecture=...}`;
the parallel prefix adders CIRCT picks for >= 8 bits are large and
redundant), 41,262 with fraig too.

97 properties (`results/core-env-ws2cmp`): mean ANDs/latches ws2
46,170/2,535, ws2sc 41,704/2,523 (sc 41,936/2,522); sc time after ws2 188 s
(354 s alone). Solved base/sc/ws/ws2/ws2sc: abc-bmc3 77/77/77/75/78 (the two
lost need 132-147 s on base, at the 150 s limit), rbmc 72/73/72/72/73 (ws2
vs base gm 33.5→27.0 s, hard 64.8→51.9), ric3-ic3-bare 59/63/57/52/62 (ws2
vs base 5.56→3.04 s on the common ones, 9 lost and 2 gained). Bare IC3
varies too much for one run: a_wb_ready_exception goes from 7 s to a
timeout on the same kind of model under load, so seeded runs
(`ric3-ic3-bare-sN`) follow. No configuration contradicts another.

## Ripple-carry adders (2026-11-18)

`ADDER=RIPPLE-CARRY` (rIC3 dropped). Mean ANDs (base / after sc, sc time):
default 56,719 / 41,936 (354 s); r 54,624 / 41,069 (276 s); ws2 46,170 /
41,704 (188 s); ws2r 44,123 / 40,840 (156 s). Results in
`results/core-env-rcmp`.

| backend | base | r | sc | rsc | ws2 | ws2r | ws2sc | ws2rsc |
|---|---|---|---|---|---|---|---|---|
| abc-bmc3 | 77 | 77 | 77 | 77 | 75 | 76 | 78 | 78 |
| rbmc | 72 | 72 | 73 | 74 | 72 | 72 | 73 | 76 |

Geometric means (common, hard): rbmc base→ws2r 33.5→25.3 s (64.8→47.9),
ws2→ws2r 27.0→25.3, sc→ws2rsc 22.7→22.0 with 3 more solved; abc-bmc3
base→ws2r 11.1→9.7. Ripple-carry alone barely changes solving (2%), but
helps after ws2 (6-8%) and makes sc 2.3x cheaper with ws2 (156 s vs 354 s)
for the smallest models (40,840) and the most solved (rbmc 76).

## PicoRV32 with ws2r (2026-11-19)

The 72 insn/csrw riscv-formal checks (K=21, 150 s, all four configurations
in one run; `results/picorv32/results-ws2r.tsv`). Mean ANDs/latches: base
34,430/2,211, sc 26,324/2,149, ws2r 32,149/2,160, ws2rsc 26,001/2,149; sc
time 38.2 s alone, 28.4 s after ws2r. Solved base/sc/ws2r/ws2rsc: abc-bmc3
63/64/64/64, rbmc 68/69/66/68 (the two lost are at the time limit).
Geometric means on the common ones: abc-bmc3 base→ws2r 43.3→32.0 s (sc 40.1),
sc→ws2rsc 40.1→29.8; rbmc base→ws2r 62.2→52.6, sc→ws2rsc 50.8→43.9.

On insn_add: kb finds nothing (no sequential constants), fraig alone gives
most of sc (base 34,271, +fraig 28,765, sc 26,407 after &scl); the merged
gates are mostly an outer mux equal to an inner value (2,373 of 3,143,
e.g. the register file's first-cycle symbolic value mux) and x+1 adders.
ws2r removes about a quarter of sc's reduction yet gets sc's speed-up on
ABC: size is not difficulty.

## Full evaluation on briareus (2026-11-19)

xinpian-tech/solver-benchmark branch `bench/circt-wordlevel` (models from
`export_all.sh`, runner `experiment/run.py`): 1,557 models, 6,927 runs, one
CPU, 600 s, 16 GiB each. Runs 44463204548 (all), 44468883578 (bare rIC3
with the seed after `ic3`), 44469491282 (ABC: the nixpkgs 0.62 build in the
flake reports pdr UNDECIDED on properties inductive in frame 1, so the
bundled unstable-2023-10-13 build of these experiments), 44474597983
(abc-pre-bmc3: iprove when scorr leaves no latch). Merged results:
`results/briareus/results.tsv`. No verdict conflicts in any run.

ws3r = `WS=2 ADDER=RIPPLE-CARRY` (known bits with context, context
simplification twice with or-form and case splits, word congruence, trimmed
registers, ripple-carry adders); ablations: r (ripple only), kb, cs, ws3
(default adders), nowc (no word congruence).

cv32: solved, geometric mean on the checks all four solve (n)

| tool | base | sc | ws3r | ws3rsc | n |
|---|---|---|---|---|---|
| abc-bmc3 | 78 / 24.7 | 78 / 16.1 | 78 / 21.2 | 78 / 14.7 | 78 |
| abc-pdr | 37 / 4.1 | 41 / 1.4 | 39 / 1.3 | 38 / 1.4 | 33 |
| abc-pre-bmc3 | 79 / 221.6 | 79 / 139.3 | 79 / 157.2 | 79 / 131.2 | 79 |
| rbmc | 76 / 88.5 | 79 / 58.9 | 79 / 64.1 | 79 / 55.9 | 76 |
| rbmc-nopre | 71 / 96.4 | 72 / 56.9 | 72 / 65.0 | 73 / 54.5 | 71 |
| ric3-ic3 | 67 / 10.4 | 67 / 6.7 | 64 / 9.6 | 68 / 8.5 | 60 |
| ric3-ic3-bare-s0 | 60 / 14.3 | 67 / 7.2 | 64 / 7.5 | 62 / 6.2 | 56 |
| ric3-ic3-bare-s1 | 59 / 15.0 | 66 / 5.7 | 63 / 6.2 | 61 / 5.9 | 55 |
| ric3-ic3-bare-s2 | 60 / 13.4 | 66 / 5.9 | 61 / 7.7 | 65 / 6.0 | 55 |

cv32 ablation: solved / geometric mean on the checks all solve

| tool | base | r | kb | cs | ws3 | nowc | ws3r | n |
|---|---|---|---|---|---|---|---|---|
| abc-bmc3 | 78 / 24.7 | 78 / 24.6 | 78 / 23.6 | 78 / 23.0 | 78 / 23.4 | 78 / 22.2 | 78 / 21.2 | 78 |
| rbmc | 76 / 88.5 | 77 / 83.8 | 78 / 76.9 | 79 / 69.6 | 77 / 70.9 | 79 / 67.2 | 79 / 64.1 | 76 |
| ric3-ic3-bare-s0 | 60 / 11.4 | 61 / 11.0 | 62 / 7.4 | 61 / 9.4 | 62 / 5.8 | 64 / 6.8 | 64 / 5.5 | 50 |

pico: solved, geometric mean on the checks all four solve (n)

| tool | base | sc | ws3r | ws3rsc | n |
|---|---|---|---|---|---|
| abc-bmc3 | 64 / 81.0 | 64 / 72.0 | 68 / 62.8 | 68 / 53.6 | 64 |
| abc-pre-bmc3 | 67 / 126.7 | 68 / 98.4 | 66 / 103.0 | 67 / 93.6 | 65 |
| rbmc | 72 / 152.5 | 72 / 124.0 | 72 / 126.3 | 72 / 112.7 | 72 |
| rbmc-nopre | 44 / 357.6 | 60 / 249.9 | 57 / 280.1 | 65 / 228.6 | 44 |

pico ablation: solved / geometric mean on the checks all solve

| tool | base | r | kb | cs | ws3 | nowc | ws3r | n |
|---|---|---|---|---|---|---|---|---|
| abc-bmc3 | 64 / 81.0 | 68 / 67.2 | 68 / 66.3 | 68 / 61.7 | 64 / 77.9 | 66 / 61.4 | 68 / 62.8 | 64 |
| rbmc | 72 / 151.4 | 72 / 137.1 | 71 / 135.7 | 72 / 125.3 | 72 / 134.1 | 72 / 124.0 | 72 / 124.0 | 71 |

Summary: without a solver, ws3r gives most of sc's speed-up on BMC (rbmc
cv32 88.5→64.1 s vs sc 58.9, same 79 solved; abc-bmc3 pico 4 more solved
than sc) and about half of sc's gain in solved checks on bare IC3 (3 seeds:
base 59.7, ws3r 62.7, sc 66.3); ws3rsc is the best BMC configuration. With
rIC3's own front end ws3r loses 3 (seed variance plus one check that is
really slower). CaDiCaL's preprocessing off: pico 44 → 57 (ws3r) → 65
(ws3rsc), cv32 unchanged gap. The operator-unsharing checks: 5 of 9 on
every back end in every configuration.
