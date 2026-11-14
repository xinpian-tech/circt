# B: compile once, simplify many times

Branch `exp/compile-once-simplify-many`, started 2026-11-14 from
`exp/synth-for-solver` (300af2c3d). Status: idea, nothing implemented.
Work starts on branch `exp/solver-oriented-simplification` (A) first.

## Idea

Classify the simplifications by the information they depend on, and use the
classification to decide what to redo when the verification problem
changes: after one compilation of the design, changing an assumption, the
reset, a cut point or the property reruns only the simplifications that
depend on what changed.

The classification follows yaoshi `docs/idea/simplification-layers.md`:

1. Information a simplification depends on: design; cut points; clock;
   reset and initial values; assumptions; property; engine.
2. Equivalence it keeps: every signal in every cycle; the result of the
   property under the problem definition; the result of one property.
3. Representation level: word-level IR, AIG, CNF.
4. Result mapping: counterexample, depth, certificate, report.

The simplifications themselves are existing ones; no new algorithm is
needed for this idea. That is the difference from A, which is about
simplifications that are better for solvers.

## What exists (exp/synth-for-solver)

- Passes for the design, clock, reset, assumption and property layers:
  experiments/synth-solver/REVIEW.md. None for cut points.
- All experiments compile one model per assertion from scratch (compile
  once, prove once). No pass records which facts it used, nothing is
  reused between assertions.
- Signal correspondence depends on reset and initial values only, but ran
  once per assertion (97 times, about 316 s each).
- Adding the environment assumptions of CV32E40X removed all 44
  counterexamples, so changing assumptions is a real operation.

## What the idea needs

- A pipeline ordered by dependency (design, then reset, then assumptions,
  then property), with the IR saved after each layer.
- Each pass reports the facts it used (which assumptions, which initial
  values); a change invalidates only the results that used the changed
  fact. Adding an assumption invalidates nothing, it only leaves
  simplification unused; removing one invalidates what used it.
- Reuse must give the same IR as recomputation.
- Constraint and vacuity checks before assumptions are used to simplify.
- A decision on cut points, which also affect the design layer.

## First experiment (not started)

On CV32E40X: (1) 97 properties sharing the design and reset layers, against
one compilation per property; (2) the OBI environment assumptions switched
on and off, rerunning the assumption and property layers only, against a
full recompilation and against no simplification with assumptions.
Measured: total simplification time, solve time, agreement of results.

## Risks

- Fact tracking is coarse for the pass that matters most: an inductive
  proof uses all initial values and constraints unless a core is extracted.
- Ordering by dependency runs the reset layer on the whole design, before
  the cone of influence of a property.
- There is no public benchmark of proof sessions; scenarios have to be
  built.
