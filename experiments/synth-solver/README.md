# Which CIRCT simplifications matter to model checkers?

`gen.py` writes small sequential models in HW/Comb/Seq MLIR. Six are miters
of the CombRewriter rewrite patterns (ASP-DAC 2026): two implementations of
the same word-level function feed two accumulators from free inputs, and
bad = accA != accB, so each induction query contains the combinational miter.
`counter` is control only. `run.sh` lowers each model through one CIRCT
configuration to AIGER and runs rIC3 (IC3 with and without its
preprocessing, k-induction) and ABC PDR (30 s limit). Tools: circt 1.147.0
(nixpkgs), only existing passes and command-line tools.

Configurations: `raw` (comb-to-synth only), `canon` (canonicalize+cse, then
raw), `lower` (circt-synth up to comb lowering), `synth` (full circt-synth,
timing strategy), `synth-area`, `synth-nodp`, `synth-fr` (functional
reduction), `synth-sop`, `raw+dc2`, `synth+dc2` (ABC dc2 after).

## Run 1 (results-run1.tsv): widths 4, 8, 12

- Word-level canonicalization decides `mulcomm` (x*y vs y*x) and `reorder`
  ((m*b)*a vs (m*a)*b): it sorts and flattens operands so both sides become
  the same node. raw at w=12 times out in every engine; canon solves in 0.05 s.
  Bit-level passes cannot recover this: raw+dc2 still times out.
- The same canonicalization hurts `hoist` (s ? ab*c : ab*d vs ab*(s?c:d)):
  it flattens mul(mul(a,b),c) into mul(a,b,c), so the shared product a*b is
  no longer a common cut point of the two sides. w=8: raw solves (ric3 15 s,
  kind 6 s, pdr 4 s), every canonicalized configuration times out.
- Patterns that CIRCT does not rewrite (`hoist`, `xorintro`, `exch`, `shift`)
  stay miters; `xorintro` w=12 is out of reach of IC3/PDR (k-induction 3 s).
- After word-level canonicalization the rest of the pipeline (datapath,
  AIG passes, functional reduction, SOP balancing, dc2) changes little;
  SOP balancing is slightly worse.
- Without any simplification (`raw`), models with nothing to merge
  (`counter`, `exch`, `shift`) are as easy as synthesized ones.
- Sub-second differences of ric3-ic3 at w=8 (0.7 s vs 0.02 s bare) are rIC3
  preprocessing overhead, not search.

## Run 2 (results-run2.tsv): solver-oriented canonicalization

`solvercanon.py` (prototype, Python on the gen.py subset) rebuilds the model
as a hash-consed word-level DAG: canonical associative-commutative chains
(sorted operands, rebuilt as binary chains, so shared sub-products stay),
mux push-down through any op, identity push-down for add/xor/or, and
mux-of-negation to xor. `--check` compares one step with and without the
rules by random simulation (a deliberately broken rule is caught). Configs
`sc` (then raw lowering), `sc+synth` (then circt-synth), and `sc-noRULE`
ablations. Widths 4-16, 30 s limit, 112 runs per config:

| config | solved | PAR-2 (s) |
|---|---|---|
| raw | 79 | 18.9 |
| canon | 94 | 10.2 |
| synth | 94 | 10.3 |
| sc | 112 | 0.1 |
| sc+synth | 112 | 0.2 |
| sc-nopush | 98 | 8.5 |
| sc-noident | 112 | 0.1 |
| sc-nonot | 112 | 0.1 |
| sc-noassoc | 100 | 6.5 |

- Mux push-down is what makes `hoist`, `xorintro` and `shift` identical on
  both sides; associative canonicalization does it for `reorder` and
  `mulcomm`. The identity and negation rules are redundant here (push-down
  and the remaining small miter cover them).
- `sc` is often larger than `synth` (counter w=16: 481 vs 148 ANDs), so the
  solver-oriented form is not the area-oriented one.
- Caveat: the models are built from exactly the patterns the rules target;
  this shows that the rewrites are reachable only at word level and decide
  the solver, not that they occur in real designs.
