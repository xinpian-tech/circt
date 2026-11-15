// RUN: circt-opt %s --comb-unshare-operators | FileCheck %s

// The shared multiplier computes a * b when sel is true; the other
// multiplier is taken then.
// CHECK-LABEL: hw.module @SameWidth
hw.module @SameWidth(in %a : i8, in %b : i8, in %c : i8, in %d : i8,
                     in %sel : i1, out shared : i8, out spec : i8) {
  // CHECK: [[SPEC:%.+]] = comb.mul %a, %b : i8
  // CHECK: [[SHARED:%.+]] = comb.mul [[X:%.+]], [[Y:%.+]] : i8
  // CHECK: [[RESULT:%.+]] = comb.mux %sel, [[SPEC]], [[SHARED]] : i8
  // CHECK: hw.output [[RESULT]], [[SPEC]]
  %spec = comb.mul %a, %b : i8
  %x = comb.mux %sel, %a, %c : i8
  %y = comb.mux %sel, %b, %d : i8
  %shared = comb.mul %x, %y : i8
  hw.output %shared, %spec : i8, i8
}

// A wider shared multiplier with sign-extended operands and an inverted
// select: only its low bits are taken from the narrower one, operands may be
// swapped.
// CHECK-LABEL: hw.module @Wider
hw.module @Wider(in %a : i8, in %b : i8, in %c : i8, in %d : i8,
                 in %nsel : i1, out shared : i10, out spec : i8) {
  // CHECK: [[SPEC:%.+]] = comb.mul %b, %a : i8
  // CHECK: [[SHARED:%.+]] = comb.mul {{%.+}}, {{%.+}} : i10
  // CHECK: [[HIGH:%.+]] = comb.extract [[SHARED]] from 8 : (i10) -> i2
  // CHECK: [[LOW:%.+]] = comb.extract [[SHARED]] from 0 : (i10) -> i8
  // CHECK: [[NEWLOW:%.+]] = comb.mux %nsel, [[SPEC]], [[LOW]] : i8
  // CHECK: [[RESULT:%.+]] = comb.concat [[HIGH]], [[NEWLOW]] : i2, i8
  // CHECK: hw.output [[RESULT]], [[SPEC]]
  %true = hw.constant true
  %spec = comb.mul %b, %a : i8
  %sel = comb.xor %nsel, %true : i1
  %x = comb.mux %sel, %c, %a : i8
  %y = comb.mux %sel, %d, %b : i8
  %xs = comb.extract %x from 7 : (i8) -> i1
  %xr = comb.replicate %xs : (i1) -> i2
  %xe = comb.concat %xr, %x : i2, i8
  %ys = comb.extract %y from 7 : (i8) -> i1
  %yr = comb.replicate %ys : (i1) -> i2
  %ye = comb.concat %yr, %y : i2, i8
  %shared = comb.mul %xe, %ye : i10
  hw.output %shared, %spec : i10, i8
}

// Two selects; the operands of the other multiplier are zero extensions
// written differently on the two sides.
// CHECK-LABEL: hw.module @TwoSelects
hw.module @TwoSelects(in %a : i4, in %b : i4, in %c : i8, in %d : i8,
                      in %s0 : i1, in %s1 : i1, out shared : i8, out spec : i8) {
  // CHECK: [[SPEC:%.+]] = comb.mul {{%.+}}, {{%.+}} : i8
  // CHECK: [[SHARED:%.+]] = comb.mul {{%.+}}, {{%.+}} : i8
  // CHECK: [[NOT:%.+]] = comb.xor %s0, %true
  // CHECK: [[COND:%.+]] = comb.and bin [[NOT]], %s1 : i1
  // CHECK: [[RESULT:%.+]] = comb.mux [[COND]], [[SPEC]], [[SHARED]] : i8
  // CHECK: hw.output [[RESULT]], [[SPEC]]
  %c0_i4 = hw.constant 0 : i4
  %false = hw.constant false
  %c0_i3 = hw.constant 0 : i3
  %az = comb.concat %c0_i4, %a : i4, i4
  %bz = comb.concat %c0_i4, %b : i4, i4
  %spec = comb.mul %az, %bz : i8
  %a5 = comb.concat %false, %a : i1, i4
  %az2 = comb.concat %c0_i3, %a5 : i3, i5
  %xin = comb.mux %s1, %az2, %c : i8
  %x = comb.mux %s0, %d, %xin : i8
  %y = comb.mux %s0, %c, %bz : i8
  %shared = comb.mul %x, %y : i8
  hw.output %shared, %spec : i8, i8
}

// No other multiplier with these operands: unchanged.
// CHECK-LABEL: hw.module @NoMatch
hw.module @NoMatch(in %a : i8, in %b : i8, in %c : i8, in %d : i8,
                   in %sel : i1, out shared : i8, out other : i8) {
  // CHECK: comb.mul %a, %d : i8
  // CHECK: [[SHARED:%.+]] = comb.mul {{%.+}}, {{%.+}} : i8
  // CHECK-NEXT: hw.output [[SHARED]],
  %other = comb.mul %a, %d : i8
  %x = comb.mux %sel, %a, %c : i8
  %y = comb.mux %sel, %b, %d : i8
  %shared = comb.mul %x, %y : i8
  hw.output %shared, %other : i8, i8
}
