# Simplification passes: review branches

Each simplification is on its own branch of xinpian-tech/circt, based on
upstream/main 788265c13 (2026-11-10). A branch holds the pass's commit and
the commits it needs; branches marked "on X" contain all of branch X.
Every branch builds on its own and passes the Synth, Verif, Seq, LLHD,
Comb, HW, ExportAIGER and ImportAIGER tests (see "Checks" below).

The layers are those of yaoshi `docs/idea/simplification-layers.md`: what
a simplification depends on (design only; clock; reset and initial
values, i.e. reachable states; assumptions; the property). "Exact" means
every signal keeps its value in every cycle; "reachable" that it holds in
reachable states only; "property" that only the result of the property is
kept; "shifted" that counterexample depths change by a recorded amount.

Front-end commits that are not simplifications (ImportVerilog, Moore XMR,
LLHD lowering, circt-verilog --uvm-stubs) stay on
`review/synth-for-solver`. Four of its commits are older versions of the
ones listed here (register correspondence, assumptions as constraints,
AIGER initial values and bad states, split/substitute assumptions); the
branches below have the current versions.

## Design (exact)

| Simplification | Pass | Branch | Commits | Depends on |
|---|---|---|---|---|
| Merge complementary or-terms (`x&c \| x&~c -> x`, 1 bit) | canonicalize (comb.or) | `review/comb-merge-complementary-or` | f1cec4b55 | - |
| Bit-blast through word-level cycles | `synth-lower-word-to-bits` | `review/synth-lower-word-to-bits-cycles` | 1eadba2e7 | - |
| SAT sweeping without choices | `synth-functional-reduction{use-choices=false}` | `review/synth-functional-reduction-choices` | e341fd516 | - |
| SOP balancing: constant-1 cuts | `synth-sop-balancing` | `review/synth-sop-balancing-constant-cuts` | ec0887ddb | - |

## Clock (exact)

| Simplification | Pass | Branch | Commits | Depends on |
|---|---|---|---|---|
| Latch-based clock gates to `seq.clock_gate` | `llhd-raise-clock-gates` | `review/llhd-raise-clock-gates` | f3cd280ab | - |
| Gated clocks to clock enables (one clock domain) | `seq-clock-gate-to-enable` | `review/seq-clock-gate-to-enable` | 6079705d1 | - |

## Reset and initial values

| Simplification | Pass | Branch | Commits | Depends on | Kind |
|---|---|---|---|---|---|
| Resets to logic; with `initial-reset` registers with reset start in reset | `seq-lower-firreg-to-compreg` | `review/seq-lower-firreg-to-compreg` | 41755f07e | - | reachable |
| Combinational cycles rebuilt as their unique fixed point | `synth-remove-false-cycles` | `review/synth-remove-false-cycles` | 05f676b09 | - | exact; with `obligation`, reachable (an output to prove 0) |
| Register and signal correspondence (scorr) | `synth-register-correspondence{internal}` | `review/synth-register-correspondence` | 1bb6e2240 | - | reachable |
| Constant registers by ternary simulation | `synth-ternary-constants` | `review/synth-ternary-constants` | 600069063 | on register-correspondence (shared file) | reachable |
| Forward retiming (and register split) | `synth-forward-retime` | `review/synth-forward-retime` | 1dfdebb6b | - | exact |
| Transient signals (temporal decomposition) | `synth-temporal-decomposition` | `review/synth-temporal-decomposition` | 5f9092644 | on check-constraints (shared file) | shifted (`verif.target_shift`) |

## Assumptions

| Simplification | Pass | Branch | Commits | Depends on |
|---|---|---|---|---|
| Assertions to one bad output (with assumptions folded in, or kept as constraints with `constraints`) | `verif-assertions-to-output{constraints}` | `review/verif-assertions-to-output` | 3a57660ee, 38d51b9de | - |
| AIGER: latch initial values, resets rejected, bad states, constraints | ExportAIGER, ImportAIGER | `review/export-aiger-safety` | f3e63f747, c204a5e70 | - |
| Split assumed conjunctions; substitute assumed inputs | `verif-split-assumes`, `verif-substitute-assumed-inputs` | `review/verif-assume-inputs` | 6d1923758 | - |
| Constraint conflicts (`:noConflict`) | `synth-check-constraints` (analysis) | `review/synth-check-constraints` | 537631213 | on ternary-constants (shared file) |

## Property

| Simplification | Pass | Branch | Commits | Depends on | Kind |
|---|---|---|---|---|---|
| Cone of influence (also dead register cycles) | `hw-cone-of-influence` | `review/hw-cone-of-influence` | 91c09e068 | - | property |
| Observability don't-cares | `verif-simplify-odc` | `review/verif-simplify-odc` | 000743a40 | - | property |
| Target shift (target enlargement) | `verif-shift-target` | `review/verif-shift-target` | 36ad65783 | - | shifted (`verif.target_shift`) |

## Checks

Each branch was built on its own (CIRCT against the LLVM build of the
experiments, which is the LLVM of upstream/main a17069fea) and tested with
lit on test/Dialect/{Synth, Verif, Seq, LLHD, Comb, HW} and
test/Conversion/{ExportAIGER, ImportAIGER}: no failures on any branch
(181 tests selected on review/verif-shift-target). Conflicts when picking
the commits onto upstream/main were only in the pass lists (.td) and
CMakeLists.txt, where the commits add lines; they were resolved by adding
those lines to upstream's files.

Results of the passes on CV32E40X and HWMCC: cv32e40x/README.md.
