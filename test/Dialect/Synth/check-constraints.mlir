// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-check-constraints))' --split-input-file --verify-diagnostics

// a and !a: a conflict in the initial state; the third constraint is not in
// the core.
// expected-warning @below {{constraints conflict in the initial state; every property holds vacuously}}
hw.module @Initial(in %a : i1, in %b : i1) {
  %na = synth.aig.and_inv not %a : i1
  // expected-note @below {{constraint in the conflict}}
  verif.assume %a : i1
  // expected-note @below {{constraint in the conflict}}
  verif.assume %na : i1
  verif.assume %b : i1
}

// -----

// r starts at 0 and then follows a; the constraints require r = 0 and a = 1,
// so the second cycle has no trace.
// expected-warning @below {{constraints conflict within 2 cycles; no trace is longer than 1 cycles}}
hw.module @Later(in %clk : !seq.clock, in %a : i1) {
  %init = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %r = seq.compreg %a, %clk initial %init : i1
  %nr = synth.aig.and_inv not %r : i1
  // expected-note @below {{constraint in the conflict}}
  verif.assume %nr : i1
  // expected-note @below {{constraint in the conflict}}
  verif.assume %a : i1
}

// -----

// Satisfiable: a = 1 always, r follows; the enabled constraint r -> b too.
hw.module @Fine(in %clk : !seq.clock, in %a : i1, in %b : i1) {
  %r = seq.compreg %a, %clk : i1
  verif.assume %a : i1
  verif.assume %b if %r : i1
}

// -----

// The enable is false in the initial state, so the second cycle conflicts.
// expected-warning @below {{constraints conflict within 2 cycles}}
hw.module @Enable(in %clk : !seq.clock, in %a : i1) {
  %init = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %true = hw.constant true
  %r = seq.compreg %true, %clk initial %init : i1
  %na = synth.aig.and_inv not %a : i1
  // expected-note @below {{constraint in the conflict}}
  verif.assume %a : i1
  // expected-note @below {{constraint in the conflict}}
  verif.assume %na if %r : i1
}
