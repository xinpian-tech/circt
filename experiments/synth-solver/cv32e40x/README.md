# CV32E40X modules through CIRCT to AIGER

Flow for one module of the CV32E40X core
(`/var/storage/workspaces/clo91eaf/p530jg-source-20261021/cv32e40x`):

    import.sh TOP OUT.mlir [SVA...]        circt-verilog, binds SVA modules
    lower.sh IN.mlir OUT.mlir --assert L   one safety model (circt-opt only)
    circt-synth | circt-translate --export-aiger
    ric3 check MODEL.aig ic3

`run_ex_stage.sh` runs the ex_stage baseline below. `cone.py` shows what is
left of an assertion's `bad` cone. ../WORKFLOW.md describes how to add a pass.

## What lower.sh does (CIRCT passes)

`lower.sh` replaced the Python script lower_regs.py; every step is a C++
pass on this branch:
- `llhd-combine-drives{free-undriven}`: LLHD signals assigned piecewise (per
  bit, element or field, as in a generate loop building a tree in one
  vector), also with a read-modify-write drive of the whole signal (checked
  bit by bit to pass the other parts through); undriven parts of signals
  without initial value take any value (Python used 0).
- `verif-assertions-to-output{asserts=L}`: SVA assertions (after
  lower-ltl-to-core: booleans, fixed-delay sequences and implications
  between them) and all assumptions to one output `bad`. Assertions are
  selected by their SVA label, which ImportVerilog now keeps (Python read
  it from the source line).
- `seq-lower-firreg-to-compreg`: registers with a reset start in the reset
  value (non-constant: taken in the first cycle), with a preset in the
  preset, else with any value; asynchronous resets are exact (the reset
  value is visible in the cycle where the reset is asserted; Python treated
  them as synchronous). Aggregate registers on their bits.
- ExportAIGER writes constant initial values of latches (AIGER 1.9) and
  rejects registers with a reset instead of dropping it.
- `verif-lower-symbolic-values{mode=hw-input}`: free values to inputs.

Checks: lit tests of each pass; `aigreach.py` (explicit-state reachability
on tiny AIGs); the whole-core results with the Python and the C++ lowering
(run 3 vs run 4). `cex.py` prints an rIC3 counterexample by input name.

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
- Fixed on this branch (ImportVerilog):
  - `a |=> b` was encoded as `concat(delay(a,1,0), true) |-> b`, which by
    the LTL dialect's definition of delay and concat checks b in the cycle
    where a holds (`|->`). Now `concat(a, delay(true,1,0)) |-> b`; with
    `y <= go`, `go |=> y` is UNSAT and `go |-> y` SAT (aigreach.py).
  - Clocking and `disable iff` inside a named property (`property p;
    @(posedge clk) disable iff (r) ...; endproperty; assert property (p)`)
    were rejected ("unsupported expression: DisableIff").
  - Covergroup declarations were rejected as module members.
  - Hierarchical references into a sibling subtree (bind ports of
    controller_fsm SVA) were left as dangling ports of the top module, and
    interface members or non-reference values gave instance operands of the
    wrong type.
  - `$stable` / `$changed` of unpacked arrays and structs were rejected
    (debug_triggers SVA): they now compare the concatenated bits.
  - Constant unpacked arrays of packed structs were built with integer
    elements, giving an invalid `hw.array_slice` (mpu SVA, `PMA_CFG[i]`):
    elements now keep the declared element type.
- `hw-imdce` crashes on the lowered core; `hw-cone-of-influence` (new) is
  used instead.

## Whole core

`import_core.sh OUT.mlir` imports the core with all its SVA (none
left out since 2026-11-07; before, debug_triggers and mpu were dropped): `make_wrapper.py` writes bhv/cv32e40x_wrapper.sv without
core_log and rvfi (they read signals by hierarchical name), and
sv/cv32e40x_core_top.sv instantiates the eXtension interface for the
wrapper's interface ports (X_EXT = 0). 197 assertions; the lowering handles
181 (boolean or fixed-delay sequences on both sides of `|->`; it skips
`until`, goto repetition and sequence `or`, 6 assertions).

The LLHD signals that blocked the AIGER export were fixed in CIRCT (CSR
bits and register-file words written by separate `always_ff`s: llhd-mem2reg
hoists their projections; probed projections in llhd-sig2reg; array slices
in `always_comb`), the clock gate is modelled open
(sv/cv32e40x_clock_gate_formal.sv), and hierarchical references between
sibling subtrees (bind ports) are wired by ImportVerilog. The export
pipeline (`run_core.sh`) is hw-cone-of-influence, hw-aggregate-to-comb,
comb-split-word-cycles (word-level false loops: struct fields depending on
each other) and the synth lowering.

One cycle of bits is left, wb_stage <-> controller_fsm: `instr_valid` in
wb_stage uses `!halt_wb && !kill_wb`, `wb_valid` depends on it,
`pending_single_step` depends on `wb_valid`, and the FSM branch it selects
drives `halt_wb`/`kill_wb` (controller_fsm.sv:461, 665, 1064; the RTL notes
that debug entries halt WB, which pulls `wb_valid` and the step request
low). It is 403 bits after bit-blasting, with 5 cut bits, and stays with
DBG_NUM_TRIGGERS = 0.

`synth-remove-false-cycles` (Synth, after convert-comb-to-synth) handles it:
- Combinationally the cycle can close: the SAT check "F^k(X) does not
  depend on the cut bits X" fails up to k = 64, and the counterexample only
  sets registers, i.e. it needs states the design does not reach.
- Iterating F does not converge even in reachable states (obligation with
  k = 1..8 is SAT): the cycle is not constructive (ternary simulation would
  not settle).
- It has exactly one consistent assignment in every reachable state: with
  `obligation fixpoint-obligation` the cut bits are computed as the unique
  v in {0,1}^5 with F(v) = v (32 copies of the cycle logic), and the
  obligation output (1 unless exactly one v matches) is proved 0 by rIC3 in
  1.5 s. By induction over the run, the rebuilt model then equals the
  original. run_core.sh splits each exported AIG into the model and its
  obligation (aigsel.py) and reports INVALID for models whose obligation is
  not proved.

### Run 3 (results-core-odc-run3.tsv), exact

266 assertions (258 labels), rIC3 IC3, 60 s. All 763 obligations proved
(35 models do not contain the cycle).

| mode       | PROVED by pass | UNSAT | SAT | TO |
|------------|---------------:|------:|----:|---:|
| none       |              0 |   165 |  73 | 28 |
| antecedent |             14 |   156 |  74 | 22 |
| full       |             14 |   155 |  70 | 27 |

- No disagreement between modes; no assertion proved in run 2 is SAT now,
  and 43 of run 2's SAT results (spurious, from the cut points) are UNSAT.
- `antecedent-only` solves 6 assertions that time out without it, loses
  none: a_mul_result (13 s) and a_shift_result_ll/lh/hl/hh (proved by the
  pass or 6-10 s), plus one SAT. The full pass gains 7 and loses 6 (all
  results at 27-47 s of the 60 s limit, i.e. noise).
- On assertions solved with and without the pass, the time is about the
  same (UNSAT: 1329 s vs 1353 s; SAT: 963 s vs 932 s).
- The SAT results are counterexamples under an unconstrained environment
  (bus, interrupts, debug); they need environment assumptions to mean
  design bugs.

### Run 4 (results-core-odc-run4.tsv), C++ lowering

Same as run 3 with lower.sh (CIRCT passes only) instead of lower_regs.py:
exact asynchronous resets, assertions selected by their SVA label, and
plain sequence properties supported. 265 labels; all 727 obligations
proved.

| mode       | PROVED by pass | UNSAT | SAT | TO | unsupported |
|------------|---------------:|------:|----:|---:|------------:|
| none       |              0 |   156 |  70 | 32 |           7 |
| antecedent |             20 |   141 |  73 | 24 |           7 |
| full       |             20 |   140 |  68 | 30 |           7 |

- Compared with run 3: no assertion changes between SAT and UNSAT/PROVED,
  and no disagreement between modes. 14 assertions the solver proved in
  run 3 are now proved by verif-simplify-odc itself (the piecewise signals
  are now merged before it runs, so it sees through them).
- The 7 unsupported assertions (`until`, goto repetition, sequence `or`)
  are reported as errors when selected; lower_regs.py skipped them
  silently, so they were not in run 3.

### Run 5 (results-core-odc-run5-new.tsv), debug_triggers and mpu SVA

With the ImportVerilog fixes for `$stable` of unpacked arrays and constant
arrays of packed structs, the SVA of debug_triggers and mpu imports (no
SVA is left out any more). The 17 assertions that are new compared with
run 4, same setup (TL = 60 s); all obligations proved:

| mode       | PROVED by pass | FOLDED | UNSAT | SAT | TO |
|------------|---------------:|-------:|------:|----:|---:|
| none       |              0 |      0 |    14 |   0 |  3 |
| antecedent |              0 |      1 |    13 |   0 |  3 |
| full       |              2 |      1 |    11 |   1 |  2 |

- FOLDED: the assertion is folded to an assert of true during the
  lowering (canonicalize after verif-simplify-odc and lower-ltl-to-core),
  without a remark of the pass. lower.sh used to report "no assertion
  labeled" for these; it now prints "folded to true by the lowering".
  a_dm_region_nonatomic_dbg is FOLDED with ODC and UNSAT (4 s) without.
  14 assertions of runs 3 and 4 are folded in all modes (e.g.
  a_lsu_no_xif_req_if_xext_disabled with X_EXT = 0); they were missing
  from the lists of those runs and are now listed as FOLDED.
- a_tdata1_writes_unique is SAT with full ODC (48 s) and TO otherwise;
  BMC finds a counterexample at depth 30 in both the none and the full
  model, so the result agrees. As for the other SAT results, the
  environment is unconstrained.

### Run 2 (results-core-odc-run2.tsv), with cut points (superseded)

Same setup, but the remaining cycle was cut with free inputs (6 per model,
in 763 of the 798 models), an option of comb-split-word-cycles that has
since been removed because it changes the model: it over-approximates, so the
UNSAT results are valid, but SAT results are not (the two counterexamples
checked, a_mul_valid/none and a_shift_result_hh/full, depend on the cut
inputs). The environment (bus, interrupts, debug) is also unconstrained.

| mode       | PROVED by pass | UNSAT | SAT (unreliable) | TO |
|------------|---------------:|------:|-----------------:|---:|
| none       |              0 |   119 |              124 | 23 |
| antecedent |             14 |   111 |              122 | 19 |
| full       |             14 |   111 |              121 | 20 |

- The pass proves 14 assertions without a solver run. a_shift_result_lh/hl
  and a_irq_step_{noetrig,ebreak}_debug_cause go from timeout to 3-5 s.
- On the assertions proved in every mode, total solve time is about the
  same (435 s -> 421 s).
- The full pass and `antecedent-only` give nearly the same results; the
  nested don't-cares rewrite more (88 vs 32 assertions) without effect on
  the solver here.

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

Soundness: every rewrite gives a property equal to the original in every
cycle (each copy equals its original wherever its facts hold; the case split
is Shannon expansion on a 1-bit signal), so assertions and assumptions keep
their meaning; only properties of the module body are handled (graph region),
and only properties without sequences (`a`, `c` 1-bit, same cycle).

When an assertion's `c` folds to true the pass emits a remark; `lower.sh --odc
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
