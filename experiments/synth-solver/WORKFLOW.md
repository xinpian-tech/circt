# Workflow: adding a solver-oriented pass to CIRCT

How the experiments in this directory are run, and the steps to take a new
simplification from an idea to a tested CIRCT pass with solver results.
`verif-simplify-odc` (lib/Dialect/Verif/Transforms/SimplifyODC.cpp) was done
this way and serves as the example.

The objective is solver time (rIC3 IC3 on the AIGER model), not AND count:
fewer ANDs can be much slower (circt-synth's datapath lowering, see
cv32e40x/README.md).

## 1. Environment

- `source env.sh` puts the local CIRCT build (`build/bin`) and the Python
  bindings on the path. Build with the toolchain of `toolchain.nix`:

      nix-shell experiments/synth-solver/toolchain.nix --run \
        "ninja -C build -j16 bin/circt-opt bin/circt-synth"

- The build has CaDiCaL (`cmake -DCIRCT_CADICAL_ENABLED=ON build`, fetched
  by CMake from GitHub), needed by synth-remove-false-cycles and the
  `cadical` lit feature.
- Store paths are not GC roots. If the compiler in `build/CMakeCache.txt`
  is gone, running `nix-shell toolchain.nix` fetches it back to the same path
  (same nixpkgs). If ninja wants to rebuild everything after that, run
  `ninja -C build -t restat` first. Do not add GC roots. If rIC3 fails with
  "required file not found", its loader or libraries were collected: `strings
  ric3 | grep /nix/store` lists them, `nix-store -r PATH` fetches each back
  (no root).
- Tools the machine lacks come from nixpkgs: `nix-shell -p
  python3Packages.yapf --run "yapf -i FILE"`, likewise for others.
- rIC3: `yjh-workspace/rIC3-target-rebuild-20261106/release/ric3` (built from
  a copy of the rIC3 source without `.git`; kissat's build script calls git).
  ABC is not available.
- Lit tests need FileCheck: `ninja -C build FileCheck count not split-file`,
  then `build/bin/llvm-lit -v test/Dialect/Verif/`.

## 2. From RTL to models

For CV32E40X (cv32e40x/README.md has the details and known gaps):

    cv32e40x/import.sh cv32e40x_ex_stage ex.mlir ex_stage_env mult div
    cv32e40x/import_core.sh core.mlir [DROP]    # whole core, all SVA
    cv32e40x/lower.sh ex.mlir MODEL.mlir --assert a_mul_result \
        [--odc full|antecedent] [--pre-cache PREFIX]
    cv32e40x/run_configs.sh MODELDIR ASSERT... > results.tsv

- `import.sh` runs circt-verilog on the core and binds the SVA modules.
- `lower.sh` makes one model per assertion with circt-opt only: LLHD
  signals, SVA -> one `bad` output (assumptions folded in), registers with
  explicit initial values (see cv32e40x/README.md). Its PRE pipeline is
  where verif-level passes go (before `lower-ltl-to-core`).
- `run_configs.sh` lowers each model with several configurations (`raw`,
  `canon`, `synth`, `synth-area`, `synth-nodp`, `sc`, ...), exports AIGER and
  runs rIC3 with and without its own preprocessing. Output TSV: assert,
  config, engine, latches, ands, result, seconds. Set `CFGS`, `TL`, `P`.
- `cone.py MODEL.mlir [--print]` shows what is left of an assertion's
  `bad` cone (size, multipliers), `cex.py` decodes a counterexample,
  `aigreach.py` checks tiny AIGs exhaustively.

Known traps: ExportAIGER used to drop register resets (now an error; lower
them with seq-lower-firreg-to-compreg);
MLIR CSE does not merge ops that differ in `sv.namehint`; `hw-imdce` crashes
on lowered designs (use `hw-cone-of-influence`). circt-verilog used to encode
`a |=> b` as `|->`; fixed on this branch. Frontend gaps found so far are
listed in cv32e40x/README.md ("CIRCT issues"); fix them in CIRCT rather than
working around them in scripts when they block an experiment.

## 3. Finding what to simplify

1. Baseline: run all assertions of a module (`run_ex_stage.sh`) and keep the
   ones that time out or are slow.
2. Compare existing configurations on them (`run_configs.sh`). If a
   configuration makes things worse, bisect the pipeline: dump IR after each
   pass (`--mlir-print-ir-after-all --mlir-print-ir-module-scope`), finish
   each snapshot with the raw lowering, solve with 3 seeds
   (results-acc_init_zero-synth-bisect.tsv).
3. Look at the hard cones (`cone.py --print`) and write down why the
   solver struggles (e.g. a multiplier miter that is only an identity under
   the assertion's antecedent).
4. Prefer generalizing an existing pass to adding a parallel one: the
   antecedent substitution became the `antecedent-only` mode of the ODC
   pass. Keep the narrower mode as an option for ablations.
5. Check the idea by hand on a small CIRCT example (edit the IR, run
   `circt-opt --canonicalize --cse`) before writing code.

A Python prototype on the lowered model is fine to test an idea quickly,
but the deliverable is a C++ pass; delete the prototype once the pass
replaces it.

## 4. Writing the pass

- Place it with the dialect it reads (Verif passes in
  `lib/Dialect/Verif/Transforms/`), declare it in the dialect's `Passes.td`
  (summary, description, options, statistics), add it to the
  `CMakeLists.txt`.
- Rewrites must be equivalence-preserving for the property being checked.
  No abstractions in passes (e.g. cutting a loop with a free input): they
  make SAT results unreliable. If one is needed for an experiment, keep it
  out of the pass and label the results.
- A rewrite that is exact only in reachable states is allowed as a proof
  obligation: the pass adds an output that is 1 where the rewrite may be
  wrong, the flow proves it 0 with the model checker, and results of models
  whose obligation is not proved are not used (synth-remove-false-cycles
  `obligation`, run_core.sh: INVALID).
- For passes on small logic, fuzz against brute force
  (fuzz_remove_false_cycles.py).
- Do not touch the design logic that next states depend on: copy what you
  rewrite and only rewrite logic used by the property alone. Rewriting the
  next-state logic (e.g. narrowing a shared multiplier) grew the AIG by 60%
  and slowed things down.
- Reuse CIRCT's canonicalization (collect the patterns in `initialize`, run
  `applyOpPatternsGreedily` on the ops you own with
  `GreedyRewriteStrictness::ExistingAndNewOps`); add only the patterns that
  are missing.
- Never drop a property silently: emit a remark when it is proved, and make
  the flow report it (lower.sh turns the remarks, which name the SVA label,
  into "proved by ...").
- `clang-format -i` the C++ (`clang-format` is in the toolchain shell).

## 5. Testing

- A lit test in `test/Dialect/<Dialect>/` with positive cases, a negative
  case (a wrong implementation must not be proved), cases the pass must
  leave alone, and `--verify-diagnostics` for the remarks.
- Run the dialect's lit directory; run `check-circt` before proposing the
  pass upstream.
- On the real flow: the models built without the new option must be
  byte-identical to the previous ones (check after pipeline changes); results
  with the pass must agree with those without (no UNSAT turning SAT or the
  reverse). For stronger evidence, inject bugs into the RTL and check that
  the affected assertions are not proved and get counterexamples.

## 6. Measuring

- Run `run_configs.sh` on the models with and without the pass, at least
  `raw` and `synth-nodp`, both rIC3 engines, 300 s.
- Single runs vary (one accumulator proof: 99-285 s across runs); repeat
  with several seeds before claiming small differences.
- Keep the TSV in the design's directory (`results-<module>-<what>-runN.tsv`)
  and summarize it in that README: what was solved, what was not and why.

## 7. Recording

- Update the design README (method, results table, limits) and
  `PROGRESS.md`.
- `yapf -i` changed Python files (repo style: `.style.yapf`).
- Commit on `exp/synth-for-solver` (author `Claude <noreply@anthropic.com>`,
  as the earlier commits), with the co-author line, and push to `origin`
  (xinpian-tech/circt).
