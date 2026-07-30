// RUN: circt-opt %s --lower-ltl-to-core | FileCheck %s --implicit-check-not=seq.compreg

// CHECK-LABEL: hw.module @UnboundedClockedDelay(
// CHECK-NOT:     seq.compreg
// CHECK:         %[[ATOM:.+]] = ltl.clocked_atom
// CHECK:         %[[DELAY:.+]] = ltl.clocked_delay %[[ATOM]], posedge %{{.+}}, 1 : !ltl.sequence
// CHECK:         verif.assert %[[DELAY]] : !ltl.sequence
hw.module @UnboundedClockedDelay(in %clock : !seq.clock, in %a : i1) {
  %0 = seq.from_clock %clock
  %1 = ltl.clocked_atom %a, posedge %0 : i1
  %2 = ltl.clocked_delay %1, posedge %0, 1 : !ltl.sequence
  verif.assert %2 : !ltl.sequence
  hw.output
}

// CHECK-LABEL: hw.module @LengthOverflow(
// CHECK-NOT:     seq.compreg
// CHECK:         %[[ATOM:.+]] = ltl.clocked_atom
// CHECK:         %[[DELAY:.+]] = ltl.clocked_delay %[[ATOM]], posedge %{{.+}}, -1, 1 : !ltl.sequence
// CHECK:         verif.assert %[[DELAY]] : !ltl.sequence
hw.module @LengthOverflow(in %clock : !seq.clock, in %a : i1) {
  %0 = seq.from_clock %clock
  %1 = ltl.clocked_atom %a, posedge %0 : i1
  %2 = ltl.clocked_delay %1, posedge %0, 18446744073709551615, 1 : !ltl.sequence
  verif.assert %2 : !ltl.sequence
  hw.output
}

// CHECK-LABEL: hw.module @UnclockedDelay(
// CHECK-NOT:     seq.compreg
// CHECK:         %[[ATOM:.+]] = ltl.clocked_atom
// CHECK:         %[[DELAY:.+]] = ltl.delay %[[ATOM]], 1, 0 : !ltl.sequence
// CHECK:         verif.assert %[[DELAY]] : !ltl.sequence
hw.module @UnclockedDelay(in %clock : !seq.clock, in %a : i1) {
  %0 = seq.from_clock %clock
  %1 = ltl.clocked_atom %a, posedge %0 : i1
  %2 = ltl.delay %1, 1, 0 : !ltl.sequence
  verif.assert %2 : !ltl.sequence
  hw.output
}

// CHECK-LABEL: hw.module @UnsupportedConcatInput(
// CHECK-NOT:     seq.compreg
// CHECK:         %[[A:.+]] = ltl.clocked_atom
// CHECK:         %[[B:.+]] = ltl.clocked_atom
// CHECK:         %[[DELAY:.+]] = ltl.delay %[[B]], 1, 0 : !ltl.sequence
// CHECK:         %[[CONCAT:.+]] = ltl.concat %[[A]], %[[DELAY]] : !ltl.sequence, !ltl.sequence
// CHECK:         verif.assert %[[CONCAT]] : !ltl.sequence
hw.module @UnsupportedConcatInput(
    in %clock : !seq.clock, in %a : i1, in %b : i1) {
  %0 = seq.from_clock %clock
  %1 = ltl.clocked_atom %a, posedge %0 : i1
  %2 = ltl.clocked_atom %b, posedge %0 : i1
  %3 = ltl.delay %2, 1, 0 : !ltl.sequence
  %4 = ltl.concat %1, %3 : !ltl.sequence, !ltl.sequence
  verif.assert %4 : !ltl.sequence
  hw.output
}

// CHECK-LABEL: hw.module @DifferentEdges(
// CHECK-NOT:     seq.compreg
// CHECK:         %[[A:.+]] = ltl.clocked_atom %{{.+}}, posedge %{{.+}} : i1
// CHECK:         %[[B:.+]] = ltl.clocked_atom %{{.+}}, negedge %{{.+}} : i1
// CHECK:         %[[A_DELAY:.+]] = ltl.clocked_delay %[[A]]
// CHECK:         %[[B_DELAY:.+]] = ltl.clocked_delay %[[B]]
// CHECK:         %[[PROPERTY:.+]] = ltl.and %[[A_DELAY]], %[[B_DELAY]]
// CHECK:         verif.assert %[[PROPERTY]] : !ltl.sequence
hw.module @DifferentEdges(in %clock : i1, in %a : i1, in %b : i1) {
  %0 = ltl.clocked_atom %a, posedge %clock : i1
  %1 = ltl.clocked_atom %b, negedge %clock : i1
  %2 = ltl.clocked_delay %0, posedge %clock, 1, 0 : !ltl.sequence
  %3 = ltl.clocked_delay %1, negedge %clock, 1, 0 : !ltl.sequence
  %4 = ltl.and %2, %3 : !ltl.sequence, !ltl.sequence
  verif.assert %4 : !ltl.sequence
  hw.output
}

// CHECK-LABEL: hw.module @UnsupportedTemporalOp(
// CHECK-NOT:     seq.compreg
// CHECK:         %[[A:.+]] = ltl.clocked_atom
// CHECK:         %[[B:.+]] = ltl.clocked_atom
// CHECK:         %[[PROPERTY:.+]] = ltl.until %[[A]], %[[B]]
// CHECK:         verif.assert %[[PROPERTY]] : !ltl.property
hw.module @UnsupportedTemporalOp(
    in %clock : !seq.clock, in %a : i1, in %b : i1) {
  %0 = seq.from_clock %clock
  %1 = ltl.clocked_atom %a, posedge %0 : i1
  %2 = ltl.clocked_atom %b, posedge %0 : i1
  %3 = ltl.until %1, %2 : !ltl.sequence, !ltl.sequence
  verif.assert %3 : !ltl.property
  hw.output
}

// CHECK-LABEL: hw.module @UnclockedConsequent(
// CHECK-NOT:     seq.compreg
// CHECK:         %[[ATOM:.+]] = ltl.clocked_atom
// CHECK:         %[[PROPERTY:.+]] = ltl.implication %[[ATOM]], %{{.+}}
// CHECK:         verif.assert %[[PROPERTY]] : !ltl.property
hw.module @UnclockedConsequent(
    in %clock : !seq.clock, in %a : i1, in %b : i1) {
  %0 = seq.from_clock %clock
  %1 = ltl.clocked_atom %a, posedge %0 : i1
  %2 = ltl.implication %1, %b : !ltl.sequence, i1
  verif.assert %2 : !ltl.property
  hw.output
}

// CHECK-LABEL: hw.module @UnclockedAntecedent(
// CHECK-NOT:     seq.compreg
// CHECK:         %[[ATOM:.+]] = ltl.clocked_atom
// CHECK:         %[[PROPERTY:.+]] = ltl.implication %{{.+}}, %[[ATOM]]
// CHECK:         verif.assert %[[PROPERTY]] : !ltl.property
hw.module @UnclockedAntecedent(
    in %clock : !seq.clock, in %a : i1, in %b : i1) {
  %0 = seq.from_clock %clock
  %1 = ltl.clocked_atom %b, posedge %0 : i1
  %2 = ltl.implication %a, %1 : i1, !ltl.sequence
  verif.assert %2 : !ltl.property
  hw.output
}

// CHECK-LABEL: hw.module @TemporalClockedAssert(
// CHECK-NOT:     seq.compreg
// CHECK:         %[[ATOM:.+]] = ltl.clocked_atom
// CHECK:         verif.clocked_assert %[[ATOM]]
hw.module @TemporalClockedAssert(in %clock : !seq.clock, in %a : i1) {
  %0 = seq.from_clock %clock
  %1 = ltl.clocked_atom %a, posedge %0 : i1
  verif.clocked_assert %1, posedge %0 : !ltl.sequence
  hw.output
}
