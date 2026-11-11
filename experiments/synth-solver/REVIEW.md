# Simplification passes: where to review them

All commits are on one branch, `review/synth-for-solver` of
xinpian-tech/circt, based on upstream/main 788265c13 (2026-11-10). Each
simplification is one commit (two where noted); "#n" is the position of
the commit on the branch (1 is the oldest). Commits only depend on earlier
ones. The branch holds only CIRCT changes; these experiments (scripts,
notes, results) are on exp/synth-for-solver only.

The layers are those of yaoshi `docs/idea/simplification-layers.md`: what
a simplification depends on (design only; clock; reset and initial
values, i.e. reachable states; assumptions; the property). "Exact" means
every signal keeps its value in every cycle; "reachable" that it holds in
reachable states only; "property" that only the result of the property is
kept; "shifted" that counterexample depths change by a recorded amount
(`verif.target_shift`).

## Design (exact)

| Simplification | Pass | Commit |
|---|---|---|
| Merge complementary or-terms (`x&c \| x&~c -> x`, 1 bit) | canonicalize (comb.or) | #2 60d6a94ac |
| Bit-blast through word-level cycles | `synth-lower-word-to-bits` | #14 c56a6b63f |
| SAT sweeping without choices | `synth-functional-reduction{use-choices=false}` | #15 b955e8976 |
| SOP balancing: constant-1 cuts | `synth-sop-balancing` | #16 feb61034c |

## Clock (exact)

| Simplification | Pass | Commit |
|---|---|---|
| Latch-based clock gates to `seq.clock_gate` | `llhd-raise-clock-gates` | #6 47c007ab2 |
| Gated clocks to clock enables (one clock domain) | `seq-clock-gate-to-enable` | #8 2dd148f08 |

## Reset and initial values

| Simplification | Pass | Commit | Kind |
|---|---|---|---|
| Resets to logic; with `initial-reset` registers with reset start in reset | `seq-lower-firreg-to-compreg` | #7 e7b2d4eb3 | reachable |
| Combinational cycles rebuilt as their unique fixed point | `synth-remove-false-cycles` | #13 63b0c4a2f | exact; with `obligation`, reachable (an output to prove 0) |
| Register and signal correspondence (scorr) | `synth-register-correspondence{internal}` | #26 332e50493; speed-up (CaDiCaL without lucky phases) #32 102898012 | reachable |
| Constant registers by ternary simulation | `synth-ternary-constants` | #27 d8360b48b | reachable |
| Forward retiming (and register split) | `synth-forward-retime` | #29 ca4b18cae | exact |
| Transient signals (temporal decomposition) | `synth-temporal-decomposition` | #30 b7de4b110 | shifted |

## Assumptions

| Simplification | Pass | Commit |
|---|---|---|
| Assertions to one bad output, assumptions folded in | `verif-assertions-to-output` | #11 a464f413c |
| ... or kept as constraints | `verif-assertions-to-output{constraints}` | #23 07a5a41e6 |
| AIGER: latch initial values, resets rejected | ExportAIGER | #12 181304392 |
| AIGER: unknown initial values, bad states, constraints (export and import) | ExportAIGER, ImportAIGER | #24 9eed00941 |
| Split assumed conjunctions; substitute assumed inputs | `verif-split-assumes`, `verif-substitute-assumed-inputs` | #25 73fcbd2a2 |
| Constraint conflicts (`:noConflict`, analysis) | `synth-check-constraints` | #28 f5ffd584b |

## Property

| Simplification | Pass | Commit | Kind |
|---|---|---|---|
| Cone of influence (also dead register cycles) | `hw-cone-of-influence` | #1 90367bc94 | property |
| Observability don't-cares | `verif-simplify-odc` | #10 2050efcf2 | property |
| Target shift (target enlargement) | `verif-shift-target` | #31 baba0f56a | shifted |

## Other commits (front end, not simplifications)

| Change | Commit |
|---|---|
| `llhd-mem2reg`: signals driven in parts by several processes | #3 6a4f79730 |
| `llhd-sig2reg`: signals read through projections | #4 4dc76ea25 |
| `llhd-combine-drives`: undriven parts, read-modify-write drives | #5 337b671ff |
| `moore.xmr` and `moore-resolve-xmr` | #9 e389b4c7d |
| ImportVerilog: `\|=>`, `disable iff` in named properties, covergroups | #17 da06f3a0f |
| ImportVerilog: statement labels of assertions | #18 1e2456488 |
| ImportVerilog: `$stable`/`$changed` of unpacked values, constant arrays of packed structs | #19 d21ea70ce |
| ImportVerilog: hierarchical names to cross-module references (needs #9) | #20 d1f4862b2 |
| ImportVerilog: `$past` with a number of cycles, of packed aggregates | #21 d94e0cac4 |
| `circt-verilog --uvm-stubs` | #22 e4a988221 |

## Checks

The branch was built (CIRCT against the LLVM build of the experiments,
which is the LLVM of upstream/main a17069fea) and tested with lit on
test/Dialect/{Synth, Verif, Seq, LLHD, Comb, HW, Moore} and
test/Conversion/{ExportAIGER, ImportAIGER, ImportVerilog}. Conflicts when
picking the commits onto upstream/main were only in pass lists (.td),
where the commits add definitions.

Results of the passes on CV32E40X and HWMCC: cv32e40x/README.md.
