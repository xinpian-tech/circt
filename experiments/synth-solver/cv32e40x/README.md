# CV32E40X modules through CIRCT to AIGER

Flow for one module of the CV32E40X core
(`/var/storage/workspaces/clo91eaf/p530jg-source-20261021/cv32e40x`):

    import.sh TOP OUT.mlir [SVA...]        circt-verilog, binds SVA modules
    lower_regs.py IN.mlir OUT.mlir ...     makes it acceptable to circt-synth
    circt-synth | circt-translate --export-aiger
    ric3 check MODEL.aig ic3

`run_ex_stage.sh` runs the ex_stage baseline below. `cone.py` shows what is
left of an assertion's `bad` cone. ../WORKFLOW.md describes how to add a pass.

## What lower_regs.py fixes

- AIGER export ignores the reset, reset value and initial value of
  seq.compreg (every latch starts at 0, no reset). Registers with a reset
  are rewritten so the AIG starts in the reset state; registers without one
  get a free initial value (`--noreset`).
- Struct/array registers, and hw.bitcast to structs that circt-synth's own
  canonicalize recreates.
- LLHD signals assigned piecewise (per bit, element or field, as in a
  generate loop building a tree in one vector) that llhd-sig2reg leaves.
- SVA assertions (after lower-ltl-to-core) to one output `bad`, with
  assumptions; labels come from the `a_name :` at the source line.

Checks: `aigreach.py` (explicit-state reachability on tiny AIGs) on small
reset/struct/assertion examples, which rIC3 agrees with; ff_one compared with
a reference on 4033 inputs. `cex.py` prints an rIC3 counterexample by input
name.

## Status (2026-11-06)

- Export works for 24 modules (alu, mult, div, ex_stage, decoders,
  prefetch_unit, alignment_buffer, write_buffer, mpu, wpt, ...).
- Not yet: register_file, csr, debug_triggers, cs_registers (several
  always_ff blocks writing elements of one array become several whole-array
  registers driving one signal), lsu_response_filter (always_comb with
  control flow left in llhd.combinational), sleep_unit (clock-gate latch),
  controller_bypass (array port), and the modules with interface ports
  (need a wrapper like the core).

## CIRCT issues found

- ExportAIGER drops compreg resets and initial values without a warning.
- circt-verilog encodes `a |=> b` as `concat(delay(a,1,0), true) |-> b`. By
  the LTL dialect's definition of delay and concat this checks b in the
  cycle where a holds (`|->`). Test: `y <= go`; `go |=> y` comes out SAT,
  `go ##1 1'b1 |-> y` UNSAT. Use the `##1` form; lower_regs.py warns.

## ex_stage baseline (results-ex_stage-run1.tsv)

ex_stage (ALU, mult, div) with the mult and div SVA bound, one model per
assertion, rIC3 IC3, 120 s, under one environment assumption
(`sv/cv32e40x_ex_stage_env_sva.sv`: id_ex_pipe holds while EX is not ready,
as the ID stage guarantees). Without it, counterexamples changed the
instruction in the middle of a MULH.

- 11 UNSAT (handshake/valid properties in < 0.2 s; the accumulator
  properties in 53 s and 91 s), 0 SAT.
- 12 time out: every multiplier result check (a_mul_result, a_mulh*_result,
  the partial-product checks a_shift_result_*) and a_check_result_constant.
  These are the datapath miters where word-level rewriting should matter.
- 366-432 latches: the environment's $stable(id_ex_pipe_i) keeps a copy of
  the whole pipeline register.

## Configuration comparison (results-ex_stage-configs-run1.tsv)

`run_configs.sh`: the 12 timeouts above plus the two accumulator
properties, 6 AIG configurations x rIC3 IC3 (default and with its own
preprocessing off), 300 s, 14 jobs in parallel.

- The 12 multiplier/constant checks time out in every configuration.
  circt-synth cuts ANDs by 13-41% (a_mulh_result 25388 -> 14922) without
  helping.
- circt-synth slows the accumulator properties: a_check_acc_init_zero is
  1-2 s on the raw AIG and 105-140 s after circt-synth (this is the 53 s of
  the baseline, which used circt-synth). Fewer ANDs, much harder for IC3.
- solvercanon.py removes only 2-5 ANDs on these models, but
  a_check_acc_mul_value_zero is fastest with it (136 s vs 185 s raw with
  IC3; with preprocessing off, 190 s vs timeout). One sample per cell, so
  only a hint.

## Which circt-synth pass slows a_check_acc_init_zero

(results-acc_init_zero-synth-bisect.tsv) IR dumped after each of the 32
circt-synth passes (`--mlir-print-ir-after-all --mlir-print-ir-module-scope`),
each snapshot finished with the raw lowering (plus convert-datapath-to-comb),
rIC3 IC3 with seeds 0-2:

- input and snapshots up to LowerVariadic{comb.mul}: 1.6-2.4 s
- from ConvertCombToDatapath (pass 5) on: 40-117 s, and no later pass
  brings it back.

So the slowdown is the datapath lowering of the one 34-bit comb.mul
(partial products + compressor tree instead of the plain array multiplier
of convert-comb-to-synth). The property only says the accumulator is 0
when a MULH starts, but the accumulator is fed by the multiplier.
`circt-synth --disable-datapath` gives 8174 ANDs (raw: 8672, default synth:
7348) and 0.6-1.1 s. Over all 14 asserts (results-ex_stage-synth-nodp.tsv)
it is as fast as raw on both accumulator properties; the 12 multiplier
checks still time out.

## Observability don't-cares (verif-simplify-odc)

The multiplier checks are datapath miters: the design's 34-bit multiplier
(operands picked by muxes on operator and FSM state, plus the accumulator)
against a multiplier written in the SVA. The CIRCT pass
`verif-simplify-odc` (lib/Dialect/Verif/Transforms/SimplifyODC.cpp)
simplifies the logic of each assertion with its observability don't-cares:
a value only has to be right where the property observes it.

1. Facts that hold where a value is observed are substituted into a copy of
   it; the design is not modified. The facts come from the enable and the
   antecedent of `a |-> c` (option `antecedent-only`, the first version of
   the pass), and inside the property from mux selects (`mux(s, x, y)`: x
   where s = 1) and and/or operands (`a & b`: b where a = 1), accumulating
   along the paths. Facts are conjuncts, negated disjuncts and `x == C`.
2. The logic used only by the assertion is canonicalized and merged with
   structurally equal logic of the cone, ignoring discardable attributes
   (CSE does not merge the two multipliers, whose `sv.namehint`s differ). An
   extra pattern cancels common add operands across `==`.
3. If `c` does not fold, case split on a 1-bit signal selecting a mux: when
   `c[s := v]` folds to true, the assertion becomes
   `(a && s != v) |-> c[s := !v]`.

When `c` folds to true the pass emits a remark; `lower_regs.py --odc
full|antecedent` runs the pass before lowering the assertions and reports
these as proved. A Python prototype of the same rewriting (on the
lowered `bad` output) gave the same models, except that a_mul_result kept
the multiplier there.

Results with `antecedent-only` (results-ex_stage-odc-run1.tsv, 300 s, raw
or `circt-synth --disable-datapath` after the pass):

| assert | before (any config) | with the pass |
|---|---|---|
| a_shift_result_ll | TO | proved by the pass |
| a_shift_result_hh / hl / lh | TO | 0.2-0.7 s |
| a_mul_result | TO | 60-146 s (raw + rIC3 with preprocessing off: TO) |
| a_check_acc_init_zero | 1-2 s | 1-2 s |
| a_check_acc_mul_value_zero | 185 s, TO with preprocessing off | 233-285 s (raw, preprocessing off: TO) |
| other 7 | TO | TO |

- a_shift_result_ll: the antecedent fixes the operand muxes, both sides
  become one product, the consequent folds to true.
- hh / hl / lh: the antecedent does not fix the operator. The case split on
  it folds the MUL_H branch; what is left needs operator == MUL_M32 in a MULH
  state, which IC3 shows unreachable.
- a_mul_result: the consequent becomes `acc[31:0] == 0` (CIRCT narrows the
  extract of the 34-bit add/mul, the extra pattern cancels the product). The
  remaining invariant is the one of a_check_acc_mul_value_zero.
- Not helped: a_mulh/mulhsu/mulhu_result and a_shift_result_ll_lh,
  ll_shift and ll_lh_hl_shift check the accumulator after 2-4 steps (a
  sequential invariant on partial sums); a_check_result_constant.
- The pass also proves a_can_receive_expediently,
  a_outputs_are_input_qualified and a_ready_o (easy for IC3 anyway).
- Applied to the next-state logic, the same rules grew the AIG by 60%
  (narrower copies of the multiplier, Python prototype): the pass only
  rewrites logic used by the assertion alone.

The full pass (don't-cares inside the property too) gives the same models
as `antecedent-only` on all 23 ex_stage assertions: their consequents are
single comparisons, and nested muxes on the same condition in the design are
already folded by canonicalize. A sweep of the other modules found no more
assertions to compare: most SVA files are written for binding in the core
(ports of other modules) or use `disable iff` inside expressions, which
circt-verilog does not support; only mult and div import on their own.
