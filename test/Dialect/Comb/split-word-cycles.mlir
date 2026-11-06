// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(comb-split-word-cycles))' 2>/dev/null | FileCheck %s
// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(comb-split-word-cycles{cut-remaining}))' 2>/dev/null | FileCheck %s --check-prefix=CUT

// A word that depends on itself, but no bit does: bit 1 is computed from bit
// 0, which comes from an input. Pushing the extracts through the mux and the
// concat splits the cycle.
// CHECK-LABEL: hw.module @FalseCycle
hw.module @FalseCycle(in %c : i1, in %a : i1, in %b : i2, out o : i2) {
  // CHECK: [[LO:%.+]] = comb.mux %c, %a, {{%.+}} : i1
  // CHECK: [[HI:%.+]] = comb.and [[LO]], %a : i1
  // CHECK: comb.concat [[HI]], %a
  %w = comb.mux %c, %x, %b : i2
  %lo = comb.extract %w from 0 : (i2) -> i1
  %hi = comb.and %lo, %a : i1
  %x = comb.concat %hi, %a : i1, i1
  hw.output %w : i2
}

// A cycle of bits is left; with cut-remaining it is cut by a new input.
// CHECK-LABEL: hw.module @BitCycle
// CUT-LABEL: hw.module @BitCycle
// CUT-SAME: in %cycle_cut_0 : i1
hw.module @BitCycle(in %a : i1, out o : i1) {
  // CHECK: comb.and {{%.+}}, %a
  // CUT: comb.and %cycle_cut_0, %a
  %x = comb.and %y, %a : i1
  %y = comb.or %x, %a : i1
  hw.output %x : i1
}
