# Rocket-chip TinyConfig: do CIRCT simplifications matter to model checkers?

Flow: rocket-chip (chisel 6.7.0) `TestHarness`/`TinyConfig` elaborated with
its mill build to FIRRTL; upstream firtool (`--lower-to-core
--disable-all-randomization -O=release|debug --ir-hw`, memories left as
seq.firmem); `prep.py` (CIRCT Python bindings) flattens the design and
builds one safety model per Chisel assert (see its docstring for the
environment: reset in cycle 0, free initial values of registers without
reset, memories of depth <= 64 as registers, the three large cache/RAM
arrays abstracted, plusarg_reader = DEFAULT), with a structural COI; `run.sh`
lowers each model through a CIRCT configuration to AIGER and runs rIC3 (IC3
with and without its preprocessing) and ABC PDR, 60 s per run.

Configurations: `raw` (comb-to-synth, word-to-bits, structural hashing),
`canon` (canonicalize+cse first), `synth`, `synth-area` (circt-synth),
`synth+dc2` (ABC dc2 after circt-synth).

## Run 2 (results-run2.tsv): 40 random asserts (seed 1), both firtool levels

All results agree across configurations (no assert is SAT in one and UNSAT in
another) and all are UNSAT. Run 1 (not kept) had 26-28 SAT results that were
artifacts of abstracting every memory (queues returned arbitrary data) and
of free plusarg values.

| opt | config | mean ANDs | ric3-ic3 | ric3-bare | abc-pdr |
|---|---|---|---|---|---|
| release | raw | 225k | 35 | 35 | 31 |
| release | canon | 200k | 36 | 36 | 31 |
| release | synth | 148k | 35 | 34 | 31 |
| release | synth-area | 148k | 36 | 34 | 30 |
| release | synth+dc2 | 133k | 36 | 34 | 30 |
| debug | raw | 247k | 35 | 34 | 31 |
| debug | canon | 195k | 36 | 36 | 30 |
| debug | synth | 145k | 37 | 36 | 31 |
| debug | synth-area | 144k | 36 | 35 | 31 |
| debug | synth+dc2 | 130k | 36 | 33 | 30 |

- The simplifications cut the AIG by up to 47% (debug raw to synth+dc2) and
  change the number solved by at most 3 of 40, in no consistent direction:
  28 asserts are solved everywhere, 3 nowhere, and the 9 others flip
  between configurations without a pattern (noise of the solvers).
- firtool's own optimization level (release vs debug) does not matter either.
- Without structural hashing the whole design lowers to 26.5M ANDs (wide
  variable shifts in the TileLink monitors), so hashing is necessary; with
  it, raw is within noise of the synthesized versions.
