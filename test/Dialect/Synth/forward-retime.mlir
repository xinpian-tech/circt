// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-forward-retime))' | FileCheck %s

// r (init 1) and s (init 0) are read only by the gate r & !s: one register
// of a_next & !b_next with initial value 1 & !0 = 1. The result then moves
// on with t (unknown init) in the next gate: init 1 & X = X.
// CHECK-LABEL: hw.module @Chain
// CHECK-DAG:     %[[N1:.+]] = synth.aig.and_inv %a, not %b : i1
// CHECK-DAG:     %[[N2:.+]] = synth.aig.and_inv %[[N1]], %c : i1
// CHECK-DAG:     %[[R:.+]] = seq.compreg %[[N2]], %clk : i1
// CHECK:         hw.output %[[R]] : i1
hw.module @Chain(in %clk : !seq.clock, in %a : i1, in %b : i1, in %c : i1,
                 out o : i1) {
  %init0 = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %init1 = seq.initial() {
    %true = hw.constant true
    seq.yield %true : i1
  } : () -> !seq.immutable<i1>
  %r = seq.compreg %a, %clk initial %init1 : i1
  %s = seq.compreg %b, %clk initial %init0 : i1
  %t = seq.compreg %c, %clk : i1
  %g = synth.aig.and_inv %r, not %s : i1
  %h = synth.aig.and_inv %g, %t : i1
  hw.output %h : i1
}

// A 0 initial value decides the gate even with an unknown one; bits of a
// wide register are split off.
// CHECK-LABEL: hw.module @Wide
// CHECK:         %[[R:.+]] = seq.compreg %{{.+}}, %clk initial %{{.+}} : i1
// CHECK:         hw.output %[[R]] : i1
hw.module @Wide(in %clk : !seq.clock, in %a : i2, in %c : i1, out o : i1) {
  %init0 = seq.initial() {
    %c0 = hw.constant 0 : i2
    seq.yield %c0 : i2
  } : () -> !seq.immutable<i2>
  %r = seq.compreg %a, %clk initial %init0 : i2
  %t = seq.compreg %c, %clk : i1
  %r0 = comb.extract %r from 0 : (i2) -> i1
  %g = synth.aig.and_inv %r0, %t : i1
  hw.output %g : i1
}

// Not moved: s is also an output; different clocks.
// CHECK-LABEL: hw.module @Kept
// CHECK:         %[[G:.+]] = synth.aig.and_inv %r, %s : i1
// CHECK:         %[[H:.+]] = synth.aig.and_inv %u, %v : i1
// CHECK:         hw.output %[[G]], %s, %[[H]]
hw.module @Kept(in %clk : !seq.clock, in %clk2 : !seq.clock, in %a : i1,
                in %b : i1, out o : i1, out p : i1, out q : i1) {
  %r = seq.compreg %a, %clk : i1
  %s = seq.compreg %b, %clk : i1
  %g = synth.aig.and_inv %r, %s : i1
  %u = seq.compreg %a, %clk : i1
  %v = seq.compreg %b, %clk2 : i1
  %h = synth.aig.and_inv %u, %v : i1
  hw.output %g, %s, %h : i1, i1, i1
}
