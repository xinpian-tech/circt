// RUN: circt-opt %s --seq-register-bisimulation | FileCheck %s

// Two registers holding the same input under the same enable are one.
// CHECK-LABEL: hw.module @dup
// CHECK-COUNT-1: seq.compreg
// CHECK-NOT: seq.compreg
// CHECK: hw.output %a, %a
hw.module @dup(in %clk : !seq.clock, in %d : i8, in %en : i1, out o : i8, out p : i8) {
  %a = seq.compreg name "a" %na, %clk : i8
  %b = seq.compreg name "b" %nb, %clk : i8
  %na = comb.mux %en, %d, %a : i8
  %nb = comb.mux %en, %d, %b : i8
  hw.output %a, %b : i8, i8
}

// Different initial values, or different inputs, keep them apart.
// CHECK-LABEL: hw.module @apart
// CHECK-COUNT-3: seq.compreg
hw.module @apart(in %clk : !seq.clock, in %d : i8, in %e : i8, out o : i8, out p : i8, out q : i8) {
  %i0 = seq.initial() {
    %c0 = hw.constant 0 : i8
    seq.yield %c0 : i8
  } : () -> !seq.immutable<i8>
  %i1 = seq.initial() {
    %c1 = hw.constant 1 : i8
    seq.yield %c1 : i8
  } : () -> !seq.immutable<i8>
  %a = seq.compreg %d, %clk initial %i0 : i8
  %b = seq.compreg %d, %clk initial %i1 : i8
  %c = seq.compreg %e, %clk initial %i0 : i8
  hw.output %a, %b, %c : i8, i8, i8
}
