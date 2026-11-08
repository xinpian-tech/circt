// RUN: circt-opt %s --pass-pipeline='builtin.module(verif-assertions-to-output)' --verify-diagnostics --split-input-file
// RUN: not circt-opt %s --pass-pipeline='builtin.module(verif-assertions-to-output{asserts=a_range})' --split-input-file 2>&1 | FileCheck %s

hw.module @TwoClocks(in %c1 : i1, in %c2 : i1, in %a : i1) {
  verif.clocked_assert %a, posedge %c1 : i1
  // expected-error @below {{needs all assertions on the rising edge of the same clock}}
  verif.clocked_assert %a, posedge %c2 : i1
}

// -----

// Unsupported properties are left out with a warning, or an error if they are
// asked for.
// CHECK: error: unsupported assertion: delay with a range
hw.module @Unsupported(in %clk : i1, in %a : i1) {
  %s = ltl.delay %a, 1 : i1
  // expected-warning @below {{assertion left out (unsupported delay with a range)}}
  verif.clocked_assert %s, posedge %clk label "a_range" : !ltl.sequence
}
