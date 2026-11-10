// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(verif-shift-target))' --split-input-file --verify-diagnostics | FileCheck %s
// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(verif-shift-target{steps=2}))' --split-input-file --verify-diagnostics | FileCheck %s --check-prefix=TWO

// p = !(r & b) with r' = a. In the next cycle r is a and b is the new input
// b_shift0; the constraint c = !d is checked there too.
// CHECK-LABEL: hw.module @Simple
// CHECK-SAME:    in %d_shift0 : i1, in %b_shift0 : i1
// CHECK-SAME:    attributes {verif.target_shift = 1 : i32}
// CHECK-DAG:     %[[R:.+]] = seq.compreg %a, %clk initial
// CHECK-DAG:     %[[P:.+]] = synth.aig.and_inv not %[[RB:.+]] : i1
// CHECK-DAG:     %[[RB]] = synth.aig.and_inv %[[R]], %b : i1
// CHECK-DAG:     %[[CN:.+]] = synth.aig.and_inv not %d_shift0 : i1
// CHECK-DAG:     %[[RBN:.+]] = synth.aig.and_inv %a, %b_shift0 : i1
// CHECK-DAG:     %[[PN:.+]] = synth.aig.and_inv not %[[RBN]] : i1
// CHECK-DAG:     %[[FIRST:shift_first]] = seq.compreg %false, %clk initial
// CHECK-DAG:     %[[F0:.+]] = synth.aig.and_inv %[[FIRST]], not %[[P]] : i1
// CHECK-DAG:     %[[F1:.+]] = synth.aig.and_inv %[[CN]], not %[[PN]] : i1
// CHECK-DAG:     %[[OK:.+]] = synth.aig.and_inv not %[[F0]], not %[[F1]] : i1
// CHECK-DAG:     verif.assert %[[OK]] : i1
// CHECK-DAG:     verif.assume %[[C:.+]] : i1
hw.module @Simple(in %clk : !seq.clock, in %a : i1, in %b : i1, in %d : i1) {
  %init = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %r = seq.compreg %a, %clk initial %init : i1
  %rb = synth.aig.and_inv %r, %b : i1
  %p = synth.aig.and_inv not %rb : i1
  verif.assert %p : i1
  %c = synth.aig.and_inv not %d : i1
  verif.assume %c : i1
}

// -----

// Two steps: the inputs of the second one are named apart.
// CHECK-LABEL: hw.module @Twice
// CHECK-SAME:    in %a_shift0 : i1)
// CHECK-SAME:    attributes {verif.target_shift = 1 : i32}
// TWO-LABEL:   hw.module @Twice
// TWO-SAME:      in %a_shift0 : i1, in %a_shift0_shift1 : i1, in %a_shift1 : i1)
// TWO-SAME:      attributes {verif.target_shift = 2 : i32}
hw.module @Twice(in %clk : !seq.clock, in %a : i1) {
  %r = seq.compreg %a, %clk : i1
  %p = synth.aig.and_inv %r, %a : i1
  verif.assert %p : i1
}

// -----

hw.module @Reset(in %clk : !seq.clock, in %rst : i1, in %a : i1) {
  %false = hw.constant false
  // expected-error @below {{cannot shift through a register with reset}}
  %r = seq.compreg %a, %clk reset %rst, %false : i1
  verif.assert %r : i1
}
