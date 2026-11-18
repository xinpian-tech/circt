// RUN: circt-opt %s --comb-split-cycles --canonicalize | FileCheck %s

// The high half of %w is computed from its low half (%a): a cycle of the
// word, not of its bits. Split, it folds to a ^ a = 0.
// CHECK-LABEL: hw.module @fields
// CHECK-NEXT:    %c0_i4 = hw.constant 0 : i4
// CHECK-NEXT:    hw.output %c0_i4
hw.module @fields(in %a : i4, out o : i4) {
  %lo = comb.extract %w from 0 : (i8) -> i4
  %hi = comb.xor %lo, %a : i4
  %w = comb.concat %hi, %a : i4, i4
  %b = comb.extract %w from 0 : (i8) -> i4
  %o = comb.extract %w from 4 : (i8) -> i4
  hw.output %o : i4
}
