// RUN: circt-opt %s --synth-lower-word-to-bits --split-input-file --verify-diagnostics | FileCheck %s

// A word that depends on itself without a cycle of bits: bit 1 is computed
// from bit 0, bit 0 from an input.
// CHECK-LABEL: hw.module @Fields
// CHECK-NOT:     unrealized_conversion_cast
// CHECK-DAG:     %[[A0:.+]] = comb.extract %a from 0 : (i2) -> i1
// CHECK-DAG:     %[[B0:.+]] = comb.extract %b from 0 : (i2) -> i1
// CHECK-DAG:     %[[X0:.+]] = synth.aig.and_inv %[[A0]], %[[B0]] : i1
// CHECK-DAG:     %[[B1:.+]] = comb.extract %b from 1 : (i2) -> i1
// CHECK-DAG:     %[[X1:.+]] = synth.aig.and_inv %[[X0]], %[[B1]] : i1
// CHECK:         %[[X:.+]] = comb.concat %[[X1]], %[[X0]] : i1, i1
// CHECK:         hw.output %[[X]]
hw.module @Fields(in %a : i2, in %b : i2, out x : i2) {
  %lo = comb.extract %x from 0 : (i2) -> i1
  %false = hw.constant false
  %a0 = comb.extract %a from 0 : (i2) -> i1
  %shifted = comb.concat %lo, %a0 : i1, i1
  %x = synth.aig.and_inv %shifted, %b : i2
  hw.output %x : i2
}

// -----

// The same through two words and a mux.
// CHECK-LABEL: hw.module @TwoWords
// CHECK-NOT:     unrealized_conversion_cast
// CHECK:         hw.output
hw.module @TwoWords(in %c : i1, in %a : i2, out x : i2, out y : i2) {
  %a0 = comb.extract %a from 0 : (i2) -> i1
  %y0 = comb.extract %y from 0 : (i2) -> i1
  %x1in = comb.concat %y0, %a0 : i1, i1
  %x = comb.mux %c, %x1in, %a : i2
  %x0 = comb.extract %x from 0 : (i2) -> i1
  %a1 = comb.extract %a from 1 : (i2) -> i1
  %y1in = comb.concat %a1, %x0 : i1, i1
  %y = comb.or %y1in, %a : i2
  hw.output %x, %y : i2, i2
}

// -----

// A cycle of bits is kept: bit 0 depends on itself.
// CHECK-LABEL: hw.module @BitCycle
// CHECK-NOT:     unrealized_conversion_cast
// CHECK:         %[[X0:.+]] = synth.aig.and_inv %[[X0]], %{{.+}} : i1
hw.module @BitCycle(in %a : i2, out x : i2) {
  %x = synth.aig.and_inv %x, %a : i2
  hw.output %x : i2
}

// -----

hw.module @WiringCycle(in %a : i1, out x : i3) {
  // expected-error @below {{bit 1 of this value is driven by itself}}
  %w = comb.concat %e, %a : i2, i1
  %e = comb.extract %w from 1 : (i3) -> i2
  %x = synth.aig.and_inv %w, %w : i3
  hw.output %x : i3
}
