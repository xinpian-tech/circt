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
    controller_fsm SVA) were left as dangling ports of the top module. Now
    ImportVerilog creates a cross-module reference (`moore.xmr`: up to the
    common ancestor, down by instance names) and `moore-resolve-xmr`
    replaces it with ports, for all instances; this also covers interface
    members of other instances.
  - `$stable` / `$changed` of unpacked arrays and structs were rejected
    (debug_triggers SVA): they now compare the concatenated bits.
  - Constant unpacked arrays of packed structs were built with integer
    elements, giving an invalid `hw.array_slice` (mpu SVA, `PMA_CFG[i]`):
    elements now keep the declared element type.
- `hw-imdce` crashes on the lowered core; `hw-cone-of-influence` (new) is
  used instead.

## Whole core

`import_core.sh OUT.mlir` imports the core with all its SVA (none
left out since 2026-11-07; before, debug_triggers and mpu were dropped),
from the original bhv/cv32e40x_wrapper.sv including core_log and rvfi (and
the SVA bound to rvfi; until 2026-11-07 make_wrapper.py removed them), with
`circt-verilog --uvm-stubs` for the UVM reporting of the SVA.
sv/cv32e40x_core_top.sv instantiates the eXtension interface for the
wrapper's interface ports (X_EXT = 0). 574 assertion ops, 338 labels.
verif-assertions-to-output handles boolean or fixed-delay sequences on both
sides of `|->`; it skips `until`, goto repetition and sequence `or`. Some
rvfi assertions do not export yet: rvfi keeps an `llhd.process` that
llhd-deseq does not turn into registers.

Clock gating (since 2026-11-07): the core runs in functional mode
(`scan_cg_en_i = 0`), with the original latch-based clock gate of the sleep
unit. `llhd-raise-clock-gates` turns the latch and AND into
`seq.clock_gate`, `seq-clock-gate-to-enable` gives the registers on the
gated clock a clock enable (exact at every edge of the ungated clock: the
latch holds the enable at the rising edge), and verif-assertions-to-output
checks assertions on the gated clock in its cycles (delays count gated
cycles; a `disable iff` also applies in the cycles in between). Before, the
gate was kept open (scan mode, `sv/cv32e40x_clock_gate_formal.sv`), so the
core clock never stopped in sleep. A sample of 40 assertions (sleep, WFI,
fencei, and random ones; results-core-functional-sample.tsv, mode none,
60 s) compared with scan mode: no assertion goes from UNSAT to SAT; two go
from SAT to UNSAT or timeout (a_branch_taken_ex_flush,
a_csr_wr_in_wb_no_fetch; their scan-mode counterexamples were not
analysed). A first version that checked `disable iff` only in gated cycles
gave a spurious counterexample for a_no_wfi_wakeup_on_wfe (a reset pulse
while the clock was gated).

The LLHD signals that blocked the AIGER export were fixed in CIRCT (CSR
bits and register-file words written by separate `always_ff`s: llhd-mem2reg
hoists their projections; probed projections in llhd-sig2reg; array slices
in `always_comb`), and hierarchical references (bind ports, rvfi) are
cross-module references. The export
pipeline (`run_core.sh`) is hw-cone-of-influence, hw-aggregate-to-comb,
bit-blasting (synth-lower-word-to-bits) and canonicalize before and after
convert-comb-to-synth, and the rest of the synth lowering.
synth-lower-word-to-bits accepts word-level cycles (e.g. a struct field
computed from another field of the same struct, across modules): such
cycles are not cycles of bits and disappear when the words are split into
bits. Upstream, the pass rejects them ("there is a combinational cycle");
before (until 2026-11-08), a separate pass comb-split-word-cycles split them
at comb level and the bit-blasting came only after
synth-remove-false-cycles. Bit-blasting and canonicalizing first makes the
models 45% smaller (a_branch_taken_ex_flush: 99877 -> 55288 ANDs, 2886 ->
2883 latches) and the remaining cycle of bits 132 bits instead of 403. The
gain comes from canonicalizing at bit level: with comb-split-word-cycles kept
and only a bit-level canonicalize added, it is 67342 ANDs. ABC (`miter`,
`dprove`) proves the old and new a_branch_taken_ex_flush models sequentially
equivalent. On the 40 assertions of the functional sample
(results-core-functional-sample-bits.tsv), 39 results are unchanged,
a_sleep_to_irq goes from SAT in 29 s to TO, and all obligations are UNSAT.
The total solve time of the 36 assertions solved in both runs hardly changes
(211 s -> 205 s): rIC3 does similar simplifications itself.

One cycle of bits is left, wb_stage <-> controller_fsm: `instr_valid` in
wb_stage uses `!halt_wb && !kill_wb`, `wb_valid` depends on it,
`pending_single_step` depends on `wb_valid`, and the FSM branch it selects
drives `halt_wb`/`kill_wb` (controller_fsm.sv:461, 665, 1064; the RTL notes
that debug entries halt WB, which pulls `wb_valid` and the step request
low). It was 403 bits after bit-blasting, with 5 cut bits (132 bits since
bit-level canonicalization, see above), and stays with
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
in 763 of the 798 models), an option of comb-split-word-cycles (a pass that
has since been removed as well) that was removed because it changes the model: it over-approximates, so the
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

## AIG simplification methods (results-core-aig-configs.tsv)

Do CIRCT's AIG optimizations make the whole-core models faster to solve?
`run_aig_configs.sh` exports each model once up to the AIG (the `run_core.sh`
pipeline, functional mode, mode none), runs each configuration on that AIG,
and solves the result with rIC3 (`ic3`, seeds 0, 1, 2, 60 s). 97 assertions:
the 80 that took 5 s or more or timed out in the 338-assertion run, and 17
easy ones. ABC configurations are external reference points, run on the
exported base AIG.

| config | passes |
|---|---|
| base | none |
| canon | canonicalize, cse |
| mac | synth-maximum-and-cover |
| sop | synth-sop-balancing (cuts of 4 inputs, 4 cuts per root) |
| fr | synth-functional-reduction{use-choices=false}, canonicalize |
| opt | circt-synth's optimization pipeline: cse, structural-hash, canonicalize, mac, sop |
| opt-nosop | opt without sop |
| opt-fr | opt, then fr |
| abc-dc2 / abc-fraig / abc-scorr | ABC `dc2`, `fraig`, `scorr; dc2` |

Each is followed by synth-lower-variadic, synth-lower-word-to-bits,
synth-structural-hash. PAR-2 counts an unsolved run as 120 s and does not
include the pass time; "solved" is per seed (mean, min-max over the seeds),
"any seed" counts assertions solved with at least one seed, and gained/lost
compares that with base.

| config | latches | ANDs | pass (s) | solved | any seed | PAR-2 (s) | gained / lost |
|---|---|---|---|---|---|---|---|
| base | 2891 | 56070 | 0 | 56.3 (54-58) | 61 | 55.6 (53.8-58.2) | |
| canon | 2891 | 55041 | 0.7 | 56.7 (54-58) | 62 | 55.2 (53.5-58.0) | +1 / -0 |
| mac | 2891 | 54853 | 0.9 | 54.3 (52-56) | 59 | 58.1 (56.7-60.3) | +1 / -3 |
| sop | 2891 | 60765 | 1.4 | 56.7 (56-57) | 62 | 55.8 (55.0-56.9) | +3 / -2 |
| fr | 2713 | 48447 | 180 | 56.0 (54-58) | 63 | 56.6 (55.4-58.8) | +4 / -2 |
| opt | 2891 | 66967 | 1.9 | 53.3 (53-54) | 58 | 60.6 (60.1-60.8) | +0 / -3 |
| opt-nosop | 2891 | 54008 | 1.1 | 56.3 (55-59) | 61 | 56.5 (54.5-57.7) | +2 / -2 |
| opt-fr | 2713 | 57060 | 224 | 52.7 (51-54) | 57 | 60.3 (59.0-61.9) | +1 / -5 |
| abc-dc2 | 2891 | 44257 | 1.0 | 53.7 (52-55) | 58 | 58.8 (57.8-60.6) | +0 / -3 |
| abc-fraig | 2891 | 46774 | 4.2 | 55.0 (52-57) | 60 | 56.3 (54.3-59.4) | +3 / -4 |
| abc-scorr | 2016 | 30758 | 51 | 58.3 (57-59) | 62 | 52.2 (50.9-53.7) | +5 / -4 |

- No CIRCT configuration is faster than base by more than the seed noise
  (base alone: PAR-2 53.8-58.2, 54-58 solved). The gained and lost
  assertions differ from configuration to configuration and are mostly
  assertions that base solves with some seeds only (a_wpt_wb_halt,
  a_wb_ready_exception, a_csr_wr_in_wb_no_fetch).
- Fewer ANDs do not mean faster solving: abc-dc2 has 21% fewer ANDs than
  base and is slower; sop and opt add ANDs (balancing duplicates logic) and
  opt is the slowest CIRCT configuration. rIC3 simplifies the
  combinational logic itself before IC3.
- fr removes 6% of the latches (registers whose inputs become equal) and
  14% of the ANDs, but takes 3 minutes per model (simulation and one SAT
  call per candidate pair), and does not solve faster.
- Only sequential simplification helps somewhat: ABC's scorr (register
  correspondence by induction) removes 30% of the latches and has the best
  PAR-2 (52.2) and most solved, but it takes 51 s per model, more than it
  saves here.
- No configuration gives a result different from base (SAT vs UNSAT) for
  any assertion and seed.

Bugs found on the way: synth-sop-balancing asserted on cuts whose function is
the constant 1 (fixed, 9f9d4bd85; the sop row was rerun after the fix), and
synth-functional-reduction with choices creates a cycle on these models
("Failed to topologically sort logic network"), so fr runs with
`use-choices=false` (5940e2c4d).

Next candidate: a CIRCT pass for sequential equivalence of registers (like
scorr), since that is the only kind of reduction that changed solve times.

## The same configurations measured by BMC (results-core-rbmc.tsv)

`run_rbmc.sh` checks the AIGs of the previous section with rBMC
(https://github.com/Clo91eaf/rBMC: AIG to CNF by Tseitin only, no
preprocessing, incremental CaDiCaL with stabilize=0) up to bound 20, 150 s,
and `summarize_rbmc.py` compares the configurations. rBMC has no
simplification of its own, so the front end is measured directly. A run is
SAT (counterexample; its witness checked with `aigsim -c`), K (no
counterexample up to bound 20) or TO (time limit; the bound reached is
recorded). PAR-2 counts a TO as 300 s; "common" is the time on the 77
assertions that every configuration finishes, "ratio" the geometric mean of
the time ratio to base on them. Pass times are not included (fr about 180 s,
abc-scorr 51 s per model).

| config | SAT | K | TO | PAR-2 (s) | common (s) | ratio |
|---|---|---|---|---|---|---|
| base | 44 | 37 | 16 | 73.6 | 1862 | 1.00 |
| canon | 44 | 39 | 14 | 70.3 | 1857 | 1.00 |
| mac | 44 | 37 | 16 | 74.7 | 1965 | 1.01 |
| sop | 44 | 36 | 17 | 80.0 | 2307 | 1.18 |
| fr | 44 | 38 | 15 | 69.0 | 1638 | 0.66 |
| opt | 43 | 35 | 19 | 87.9 | 2680 | 1.39 |
| opt-nosop | 44 | 39 | 14 | 70.1 | 1873 | 0.98 |
| opt-fr | 44 | 34 | 19 | 85.8 | 2488 | 0.91 |
| abc-dc2 | 44 | 40 | 13 | 65.3 | 1645 | 0.90 |
| abc-fraig | 44 | 40 | 13 | 64.4 | 1590 | 0.82 |
| abc-scorr | 44 | 40 | 13 | 58.7 | 1190 | 0.34 |

- Unlike rIC3's IC3, BMC tells the configurations apart: functional
  reduction (fr) makes BMC 34% faster on the common assertions (geometric
  mean), fraig 18%, dc2 10%, and scorr (sequential: register
  correspondence) almost 3x. rIC3 simplifies the model itself before IC3,
  which hides these differences.
- sop balancing and the opt pipeline built on it make BMC slower (1.18,
  1.39): they duplicate logic (sop adds 8%, opt 19% AND gates). opt-nosop
  is the same as base; mac and canon change little.
- Soundness: no configuration contradicts another (same counterexample
  length everywhere, no bound 20 without counterexample where another has a
  shorter one), all 483 witnesses pass `aigsim -c`, and no counterexample
  is on an assertion that rIC3 proved on the same model (in 192 runs rBMC
  finds a counterexample where IC3 timed out at 60 s).

## Register correspondence (synth-register-correspondence, results-core-rbmc-rc.tsv)

`synth-register-correspondence` merges register bits that are equal,
complementary or constant in every reachable state, proved by induction
(the register part of ABC's scorr). Configuration `rc` of
`run_aig_configs.sh`: the pass, canonicalize, and the bit-level cleanup.

| config | latches | ANDs | pass (s) |
|---|---|---|---|
| base | 2891 | 56070 | |
| rc | 2389 | 49162 | 16 (max 25) |
| abc-scorr | 2016 | 30758 | 51 |

Equivalence of rc with base: ABC `miter; dprove` proves 90 of the 97
models equivalent and times out (15 min) on 7; on those 7 miters rBMC
finds no difference (up to bound 30 on six, bound 4 on a_shift_result_ll).

rBMC, bound 20, 150 s, the three configurations in one batch (82
assertions finished by all):

| config | SAT | K | TO | PAR-2 (s) | common (s) | ratio |
|---|---|---|---|---|---|---|
| base | 44 | 38 | 15 | 70.7 | 2358 | 1.00 |
| rc | 44 | 39 | 14 | 67.4 | 2205 | 0.89 |
| abc-scorr | 44 | 40 | 13 | 58.7 | 1572 | 0.34 |

Register correspondence alone gives 11%; scorr gets 3x. scorr also merges
internal signals (signal correspondence, not only registers) and runs dc2
afterwards, so its models have 13% fewer latches and 37% fewer AND gates
than rc. No soundness problems across the three configurations.

## Signal correspondence, forward retiming

`synth-register-correspondence{internal}` also merges i1 and-gates. With
one incremental solver and chunked queries (`chunk=256`, the default),
a_gate_clk_i takes 409 s (the per-pair speculative rounds, `chunk=0`, had
not converged after 40 min):

| model      | latches | ands  |
|------------|---------|-------|
| base       | 2879    | 55278 |
| rc         | 2375    | 48376 |
| internal   | 2373    | 40548 |
| abc-scorr  | 2373    | 35555 |

ABC dprove: equivalent to base. On 40 HWMCC safety models (every fourth
of the small ones): 26 equivalent by dprove, none different; two of the
others had their property proved by the pass itself (bad merged to 0,
confirmed by ABC pdr).

`synth-forward-retime` finds nothing to move on CV32E40X (no and-gate reads
only registers); its register split alone drops unread bits: 2879 -> 2684
latches, 55278 -> 53313 ands (dprove: equivalent).

## Timing-changing passes (verif-shift-target, synth-temporal-decomposition)

Both record the shift in the module attribute `verif.target_shift`: a
counterexample of depth d is one of depth at most d + shift of the model
before, and a bounded proof to depth d covers d + shift.

`verif-shift-target` (HWMCC, ABC bmc3): arbitrated_top_n3_* fail at
18/10/10, after one step at 17/9/9, after two at 16/8/8.

`synth-temporal-decomposition{bad-outputs}` on a_gate_clk_i: 372 register
bits are constant from the start, 3 more after one cycle. Skipping that cycle
does not pay (no gate becomes constant, 10 gates of prefix logic), so the
default keeps the timing and the result is that of `synth-ternary-constants`:

| model                 | latches | ands  | pass time |
|-----------------------|---------|-------|-----------|
| base                  | 2879    | 55278 |           |
| ternary constants     | 2418    | 48767 | 0.4 s     |
| temporal dec. (force) | 2416    | 48765 | 0.5 s     |
| rc                    | 2375    | 48376 | 16 s      |

HWMCC (hwmcc/check_shift.sh: rBMC on both models, witnesses of the new model
mapped back and replayed on the original): with `force`, 121 models: 78 with
a prefix (1, 2 or 4 cycles) all consistent, 12 of them with counterexamples
(depth d becomes d - k, all witnesses replay); 40 without transients; 3 not
run (2 import errors, 1 timeout before ports were added in one batch; it
passes since). With the default policy none of 33 sampled models is shifted:
the prefix logic outweighs the few transient bits (cv32e40x-p664 with
`force`: 25 transient bits, 146510 -> 791338 ands).

ABC is not used for these checks: it starts latches without initial value
at 0. For hkust/x-epic/a16-p1 ABC pdr reports "proved" while the model fails
at depth 10 (rBMC; replayed with hwmcc/aigreplay.py).

## The new passes on all 97 assertions (results-core-rbmc-sc.tsv)

Configurations of `run_aig_configs.sh`, each followed by canonicalize and the
bit-level cleanup: `tc` (synth-ternary-constants), `split`
(synth-forward-retime; on CV32E40X only its register split acts), `rc`
(synth-register-correspondence), `sc` (the same with `internal`: signal
correspondence), `all` (forward-retime, ternary-constants, sc, then
functional reduction). 12 exports in parallel on 16 cores, so pass times
are under load. rBMC as above (bound 20, 150 s, 8 in parallel).

Sizes (mean over the 97 models) and pass times:

| config | latches | ANDs | pass (s, mean) | pass (s, max) |
|---|---|---|---|---|
| base | 2891 | 56070 | | |
| tc | 2432 | 49558 | 1.0 | 1.3 |
| split | 2699 | 54116 | 1.1 | 1.3 |
| rc | 2389 | 49162 | 477 | 1461 |
| sc | 2019 | 35042 | 1039 | 1441 |
| all | 2019 | 35042 | 984 | 1785 |
| abc-scorr | 2016 | 30758 | 51 | 82 |

sc and abc-scorr reduce 15 assertions to a constant (0 latches, 0 ANDs:
the bad output merged with 0); the same 15 for both, all of them proved by
rIC3 and without counterexample to bound 20 in every configuration. `all`
equals sc up to one AND gate on 3 models: after signal correspondence the
other passes find nothing.

rBMC (84 assertions finished by every configuration):

| config | SAT | K | TO | PAR-2 (s) | common (s) | ratio |
|---|---|---|---|---|---|---|
| base | 44 | 40 | 13 | 59.2 | 1839 | 1.00 |
| tc | 44 | 40 | 13 | 58.1 | 1731 | 0.92 |
| split | 44 | 40 | 13 | 57.8 | 1706 | 0.95 |
| rc | 44 | 40 | 13 | 57.3 | 1660 | 0.87 |
| sc | 44 | 40 | 13 | 52.6 | 1198 | 0.36 |
| all | 44 | 40 | 13 | 52.1 | 1151 | 0.35 |
| abc-scorr | 44 | 40 | 13 | 51.8 | 1122 | 0.35 |

- Signal correspondence gives BMC the same speedup as ABC's scorr (2.8x,
  geometric mean); rc 13%, tc 8%, split 5%. No configuration finishes an
  assertion that base does not.
- Soundness: the same counterexample length in every configuration, no
  bound 20 without counterexample where another has one, all 308 witnesses
  pass `aigsim -c`.
- Cost: sc takes about 20x the time of scorr (1039 s against 51 s per model
  here). ABC runs dc2 after scorr, hence its 12% fewer ANDs.

Chunk size of the incremental proof (`chunk`), a_gate_clk_i, machine less
loaded (rBMC, 8 jobs). Without `internal`, small chunks are faster; with it,
the default 256 is. The results are the same; the default stays 256.

| chunk | rc (s) | rc SAT calls | sc (s) | sc SAT calls |
|---|---|---|---|---|
| 0 (one pair at a time) | 35 | 21732 | did not converge in 40 min (earlier) | |
| 8 | 37 | 1604 | > 21 min (stopped) | |
| 32 | 32 | 1415 | 749 | 8760 |
| 256 | 125 | 1495 | 637 | 3751 |
