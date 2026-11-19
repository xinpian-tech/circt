# Section 3.2 study: where the gains of signal correspondence come from

Scripts: `/root/yjh-workspace/circt-solver-oriented/experiments/solver-oriented/study/` (export_kbd.sh, sizes.sh, sizes.py, attr_run.sh, wordir.py,
classify.py, make_summary.py). Per-check data: `sizes.tsv`, `attribution.tsv`.

## Method

Models (all with the default adders and comparators, no unsharing):

- A0: BASE (final pipeline without sc): CV32 `core-env-final/<c>.base.aig`,
  Pico `bench/pico/base/<c>.base.aig`.
- A1: A0 after ABC `&fraig -C 1000` (combinational SAT sweeping: merges
  equivalences that hold in every state, no reachability), then `&scl` again
  (latches left dangling).
- A2: known bits only ("kbd", exported here with `export_cfgs.sh` NOUS=1
  NOSC=1 XDCP="seq-known-bits{context},canonicalize,cse,seq-trim-registers,
  canonicalize,cse,", `study32/{cv32,pico}/kbd/<c>.base.aig`), then `&fraig -C
  1000; &scl`.
- A3: SC (final pipeline, `synth-register-correspondence{internal}`):
  CV32 `core-env-final/<c>.sc.aig`, Pico `bench/pico/base/<c>.sc.aig`.
- W: word-level simplification with default adders (ws3,
  `bench/{cv32,pico}/ws3/<c>.base.aig`); WF: W then `&fraig -C 1000; &scl`.

Every model is read with the bundled ABC as `read; strash; &get; &scl; &ps`
(ANDs and latches after cone of influence and dangling-latch removal);
`&fraig` uses at most 1000 conflicts per node (the default 1e6 did not
finish on the multiplier checks in 15 minutes; on a_addr_incr -C 1000 and
the default give the same model). sc's reduction A0-A3 splits into
combinational A0-A1, constant register bits A1-A2 (what word-level known
bits finds beyond fraig) and the remainder A2-A3 that needs induction.

Pattern attribution (all 97 + 72 checks, no sampling): the word-level IR
after `canonicalize,cse,hw-cone-of-influence,canonicalize,hw-aggregate-to-comb,
canonicalize` is printed, every op line gets `loc("w":LINE:0)`, it is lowered
with the rest of the final pipeline (`synth-lower-word-to-bits ...
synth-structural-hash`, default adders) and `synth-register-correspondence{internal
merge-dump merge-to=0}` prints the proved classes (merge-to=0 skips the
merges and their slow per-merge cone dumps; the classes are the same). Run
(a) on the model with every register's initial value removed (`initial %x`
deleted: registers start anywhere, so the base case requires a class to hold
in every state, i.e. only combinational merges) and (b) on the normal model.
Validation: sc with merging on the tagged IR gives exactly A3 (ANDs and
latches after &scl) on pico insn_add, insn_lw, insn_beq, csrw_mcycle and
cv32 a_addr_incr, a_no_irq_write_during_sleep (`validate.sh`).

Every non-representative member of a class (every member of a constant
class) is one merge, classified by the rules at the end. "new" merges are
those of (b) beyond (a), per source: a word op line (or register bit) with
n0 merged members in (a) and n1 > n0 in (b) contributes n1-n0, given the
categories (b) has there and (a) has not. (Subtracting the per-category
totals instead would be confounded: many gates that (a) merges as reset
muxes into another gate are constant in (b), so the reset-mux total drops
and the constant total rises with initial values.)

## Caveats

- Merge counts are not AND counts: one merged gate can remove a whole cone
  or nothing (its cone is shared), and sc runs on the model with all outputs
  (all assertions and constraints) while the sizes are measured on the
  property output 0 plus the constraints after `&scl`; some merges are in
  logic `&scl` removes anyway. The category shares describe where sc's
  merges come from, the size split describes how much they are worth.
- A1 uses a conflict limit: equivalences fraig gives up on count as
  "induction" (A2-A3). sc+&fraig equals A3 on every check, so sc's model is
  fraig-closed; the bias goes against the combinational part.
- A1-A2 (known bits after fraig) is not purely "constant registers":
  seq-known-bits{context} also simplifies with path-sensitive known bits and
  seq-trim-registers removes registers nothing reads. It is negative on 3
  CV32 checks (the kbd model fraigs slightly worse) and zero on every pico
  check. Most constant registers sc proves (const-reg / const-gate
  categories, e.g. CV32 pipeline-register fields of unused features:
  watchpoint match, mcountinhibit, irq_q, MPU state) are not found by known
  bits: they need induction.
- The noinit run frees every register, including the harness' `first` and
  pico's `reset` register, so it is strictly combinational; it may also lose
  combinational facts that the SC pass would get only via simulation
  candidates from the initial state (none observed: no source has fewer
  merges with initial values than without except 1 CV32 member).
- Unnamed registers (CV32 has ~390 registers without a name attribute, e.g.
  split CSR bits and the register file) cannot be checked for `_sva`; they
  count as reg-design. Gates fused by canonicalize (bare `G`) or carrying the
  module's location are "unlocated" (~4% CV32, ~1% pico).
- pico's first-mux category is a harness artefact: every register read is
  `first ? reg : symbolic_init`; the AND `first & reg` equals `reg` in every
  reachable state because `reg` is 0 when first is 0 (cycle 0), which needs
  induction (first == !reset).

## Sizes and the split of sc's reduction

### cv32 (97 checks)

Sums and medians over the checks (ANDs / latches after `&scl`):

| model | sum ANDs | median ANDs | sum latches | median latches |
|---|---|---|---|---|
| A0 base | 4863023 | 49366 | 248767 | 2554 |
| A1 base+&fraig | 4135048 | 41838 | 245445 | 2519 |
| A2 known bits+&fraig | 4132681 | 41810 | 245445 | 2519 |
| A3 sc | 4067286 | 41184 | 244390 | 2509 |
| sc+&fraig | 4067286 | 41184 | 244390 | 2509 |
| W ws3 | 4399040 | 44585 | 245415 | 2519 |
| WF ws3+&fraig | 4099575 | 41476 | 245285 | 2518 |

Split of sc's reduction A0-A3 (share of the summed reduction; median of the per-check shares over checks with A0>A3):

| part | ANDs sum | share of sum | median share | latches sum | share of sum | median share |
|---|---|---|---|---|---|---|
| sc total A0-A3 | 795737 | 100.0% | 100.0% | 4377 | 100.0% | 100.0% |
| combinational A0-A1 | 727975 | 91.5% | 92.0% | 3322 | 75.9% | 77.3% |
| constant register bits A1-A2 | 2367 | 0.3% | 0.3% | 0 | 0.0% | 0.0% |
| needs induction A2-A3 | 65395 | 8.2% | 7.7% | 1055 | 24.1% | 22.7% |
| WL without SAT A0-W | 463983 | 58.3% | 58.4% | 3352 | 76.6% | 77.3% |
| WL then &fraig A0-WF | 763448 | 95.9% | 96.4% | 3482 | 79.6% | 79.5% |

- ANDs: induction remainder A2-A3 negative on 0 of 97 checks; sc reduction <= 0 on 0; WL reduces more than sc on 0.
- latches: induction remainder A2-A3 negative on 0 of 97 checks; sc reduction <= 0 on 0; WL reduces more than sc on 0.

### pico (72 checks)

Sums and medians over the checks (ANDs / latches after `&scl`):

| model | sum ANDs | median ANDs | sum latches | median latches |
|---|---|---|---|---|
| A0 base | 2463848 | 34271 | 155191 | 2151 |
| A1 base+&fraig | 2065225 | 28758 | 154831 | 2146 |
| A2 known bits+&fraig | 2065225 | 28758 | 154831 | 2146 |
| A3 sc | 1895337 | 26400.5 | 154758 | 2145 |
| sc+&fraig | 1895337 | 26400.5 | 154758 | 2145 |
| W ws3 | 2246610 | 31253.5 | 155191 | 2151 |
| WF ws3+&fraig | 2063353 | 28729 | 154831 | 2146 |

Split of sc's reduction A0-A3 (share of the summed reduction; median of the per-check shares over checks with A0>A3):

| part | ANDs sum | share of sum | median share | latches sum | share of sum | median share |
|---|---|---|---|---|---|---|
| sc total A0-A3 | 568511 | 100.0% | 100.0% | 433 | 100.0% | 100.0% |
| combinational A0-A1 | 398623 | 70.1% | 70.0% | 360 | 83.1% | 83.3% |
| constant register bits A1-A2 | 0 | 0.0% | 0.0% | 0 | 0.0% | 0.0% |
| needs induction A2-A3 | 169888 | 29.9% | 30.0% | 73 | 16.9% | 16.7% |
| WL without SAT A0-W | 217238 | 38.2% | 38.3% | 0 | 0.0% | 0.0% |
| WL then &fraig A0-WF | 400495 | 70.4% | 70.4% | 360 | 83.1% | 83.3% |

- ANDs: induction remainder A2-A3 negative on 0 of 72 checks; sc reduction <= 0 on 0; WL reduces more than sc on 0.
- latches: induction remainder A2-A3 negative on 0 of 72 checks; sc reduction <= 0 on 0; WL reduces more than sc on 0.



## Pattern attribution (merged members, all checks)

### cv32 (97 checks)

Merged members: 673040 without initial values (comb: true in every state), 1091881 with (all); 418842 new merges with initial values (sources merged more often with than without), 1 lost (sources merged less often: their gates are covered by other classes, e.g. a gate merged into another gate without initial values and constant with them).

| category | comb | % of comb | new (induction) | % of new | all | % of all |
|---|---|---|---|---|---|---|
| const-gate | 32729 | 4.9% | 259749 | 62.0% | 336624 | 30.8% |
| reset-mux | 229534 | 34.1% | 692 | 0.2% | 207427 | 19.0% |
| mux-dup | 93031 | 13.8% | 55704 | 13.3% | 165808 | 15.2% |
| mux-select | 133768 | 19.9% | 27367 | 6.5% | 132226 | 12.1% |
| decode-compare | 103619 | 15.4% | 11173 | 2.7% | 105377 | 9.7% |
| unlocated | 38797 | 5.8% | 3490 | 0.8% | 42287 | 3.9% |
| const-reg | 0 | 0.0% | 39387 | 9.4% | 39387 | 3.6% |
| arith-same | 34751 | 5.2% | 293 | 0.1% | 34898 | 3.2% |
| arith-cross | 6811 | 1.0% | 19785 | 4.7% | 26645 | 2.4% |
| reg-design | 0 | 0.0% | 1164 | 0.3% | 1164 | 0.1% |
| reg-sva | 0 | 0.0% | 38 | 0.0% | 38 | 0.0% |

### pico (72 checks)

Merged members: 236819 without initial values (comb: true in every state), 444212 with (all); 207393 new merges with initial values (sources merged more often with than without), 0 lost (sources merged less often: their gates are covered by other classes, e.g. a gate merged into another gate without initial values and constant with them).

| category | comb | % of comb | new (induction) | % of new | all | % of all |
|---|---|---|---|---|---|---|
| mux-select | 152670 | 64.5% | 20592 | 9.9% | 173622 | 39.1% |
| first-mux | 0 | 0.0% | 158314 | 76.3% | 158314 | 35.6% |
| mux-dup | 25553 | 10.8% | 2232 | 1.1% | 25769 | 5.8% |
| const-gate | 9518 | 4.0% | 14256 | 6.9% | 25214 | 5.7% |
| decode-compare | 16983 | 7.2% | 4901 | 2.4% | 20300 | 4.6% |
| arith-same | 14942 | 6.3% | 0 | 0.0% | 14942 | 3.4% |
| arith-cross | 5408 | 2.3% | 72 | 0.0% | 5480 | 1.2% |
| regfile-slot | 4608 | 1.9% | 0 | 0.0% | 4608 | 1.0% |
| gate-eq-reg | 144 | 0.1% | 2346 | 1.1% | 4434 | 1.0% |
| reset-mux | 2746 | 1.2% | 1800 | 0.9% | 4402 | 1.0% |
| unlocated | 4247 | 1.8% | 144 | 0.1% | 4391 | 1.0% |
| reg-design | 0 | 0.0% | 2448 | 1.2% | 2448 | 0.6% |
| const-reg | 0 | 0.0% | 288 | 0.1% | 288 | 0.1% |



Word ops of the constant gates (with initial values):

| design | word op of the constant gate | members | % |
|---|---|---|---|
| cv32 | mux | 176543 | 52.4% |
| cv32 | mux on reset | 60552 | 18.0% |
| cv32 | unlocated | 28020 | 8.3% |
| cv32 | and | 27532 | 8.2% |
| cv32 | icmp ne | 13033 | 3.9% |
| cv32 | arithmetic | 11948 | 3.5% |
| cv32 | icmp eq | 8411 | 2.5% |
| cv32 | icmp ceq | 5427 | 1.6% |
| pico | mux | 22464 | 89.1% |
| pico | and | 1605 | 6.4% |
| pico | unlocated | 360 | 1.4% |
| pico | mux on first | 288 | 1.1% |
| pico | or | 144 | 0.6% |
| pico | mux on reset | 144 | 0.6% |
| pico | arithmetic | 144 | 0.6% |
| pico | icmp ne | 65 | 0.3% |


## Classifier rules

```
Every merged member gets exactly one category, by the first rule that
applies (M = word op of the member, R = word op of the representative):
 1 const-reg      constant class, member is a register bit
 2 const-gate     constant class, member is a gate
 3 reg-sva        member is a register bit and the member or the
                  representative is named *_sva* (assertion shadow state)
 4 reg-design     member is a register bit (other register equivalence;
                  includes unnamed registers, which print with an empty name)
 5 unlocated      member gate without a location (fused) or whose line is
                  not an op line (e.g. the module's own location)
 6 reset-mux      M is a mux whose condition reads the reset (cv32: input
                  rst_ni, pico: register reset) through at most 3
                  and/or/xor ops and one of whose data arms is a constant
 7 first-mux      M is a mux whose condition reads the harness' first-cycle
                  register `first` through at most 3 and/or/xor ops (the
                  harness' symbolic initial values, `first ? reg : sym`)
 8 regfile-slot   M is a mux that feeds (through at most 2 muxes) a
                  register named *cpuregs*/*register_file*/*regfile*, or a
                  write slot: one of its non-constant data arms (the write
                  data) is shared by muxes feeding (through at most 2
                  muxes) at least 8 different registers
 9 arith-same     M and R are the same add/sub/mul/div/mod/shift or ordering
                  comparison (bits of one arithmetic op, e.g. carries)
10 arith-cross    M or R is such an arithmetic op (different ops: e.g. the
                  xor or equality test of two operands vs. their difference)
11 gate-eq-reg    the representative is a register bit (a gate equal to a
                  register: next-state / hold logic)
12 decode-compare M and R are icmp (eq/ne)/and/or/xor (R: or unlocated):
                  equality tests, priority decoders, their conjunctions
13 mux-select     M is a mux and R is reachable from a data arm of M through
                  at most 3 muxes (redundant select: the mux, or a chain of
                  muxes, reduces to one of its arms; its condition is
                  implied by the context)
14 mux-dup        M or R is a mux otherwise (selects equal to another
                  select or gate built separately: duplicated mux trees)
15 other          anything else (e.g. concat/extract-only ops)
```
