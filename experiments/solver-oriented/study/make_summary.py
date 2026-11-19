# make_summary.py: study32/summary.md from the method text below and the
# tables written by sizes.py and classify.py.
import re

D = '/root/yjh-workspace/solver-oriented-out/study32'
S = '/root/yjh-workspace/circt-solver-oriented/experiments/solver-oriented/study'
sizes = open(f'{D}/sizes-summary.md').read()
attr = open(f'{D}/attribution-summary.md').read()
constops = open(f'{D}/const-ops.md').read()
rules = re.search(r'# Every merged member.*?(?=\nimport)', open(f'{S}/classify.py').read(), re.S).group(0)
rules = '\n'.join(l[2:] if l.startswith('# ') else l.lstrip('#') for l in rules.split('\n'))

out = """# Section 3.2 study: where the gains of signal correspondence come from

Scripts: `@@S@@/` (export_kbd.sh, sizes.sh, sizes.py, attr_run.sh, wordir.py,
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

@@sizes@@

## Pattern attribution (merged members, all checks)

@@attr@@

Word ops of the constant gates (with initial values):

@@constops@@

## Classifier rules

```
@@rules@@
```
"""
for k, v in [('S', S), ('sizes', sizes), ('attr', attr), ('constops', constops), ('rules', rules)]:
    out = out.replace('@@' + k + '@@', v)
open(f'{D}/summary.md', 'w').write(out)
print(out)
