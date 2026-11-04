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
