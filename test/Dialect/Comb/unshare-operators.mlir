// RUN: circt-opt %s --comb-unshare-operators='isolate=false widen=false' | FileCheck %s
// RUN: circt-opt %s --comb-unshare-operators=max-paths=1 | FileCheck %s --check-prefix=LIMIT
// RUN: circt-opt %s --comb-unshare-operators | FileCheck %s --check-prefix=ISOLATE

// With one assignment tried (the empty one) nothing is found.
// LIMIT-LABEL: hw.module @SameWidth
// LIMIT: comb.mul %a, %b : i8
// LIMIT: [[SHARED:%.+]] = comb.mul {{%.+}}, {{%.+}} : i8
// LIMIT-NEXT: hw.output [[SHARED]],

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

// Divisions are not commutative and are only matched at the same width.
// CHECK-LABEL: hw.module @Division
hw.module @Division(in %a : i8, in %b : i8, in %c : i8, in %d : i8,
                    in %sel : i1, out shared : i8, out swapped : i8,
                    out spec : i8) {
  // CHECK: [[SPEC:%.+]] = comb.divu %a, %b : i8
  // CHECK: [[SHARED:%.+]] = comb.divu {{%.+}}, {{%.+}} : i8
  // CHECK: [[RESULT:%.+]] = comb.mux %sel, [[SPEC]], [[SHARED]] : i8
  // CHECK: [[SWAPPED:%.+]] = comb.divu {{%.+}}, {{%.+}} : i8
  // CHECK-NEXT: hw.output [[RESULT]], [[SWAPPED]], [[SPEC]]
  %spec = comb.divu %a, %b : i8
  %x = comb.mux %sel, %a, %c : i8
  %y = comb.mux %sel, %b, %d : i8
  %shared = comb.divu %x, %y : i8
  %swapped = comb.divu %y, %x : i8
  hw.output %shared, %swapped, %spec : i8, i8, i8
}

// A shift is not taken from a multiplication, nor from a shift of another
// width.
// CHECK-LABEL: hw.module @Shift
hw.module @Shift(in %a : i8, in %b : i8, in %c : i8, in %sel : i1,
                 out shared : i8, out spec : i8, out narrow : i4) {
  // CHECK: [[SPEC:%.+]] = comb.shl %a, %b : i8
  // CHECK: [[SHARED:%.+]] = comb.shl {{%.+}}, {{%.+}} : i8
  // CHECK: [[RESULT:%.+]] = comb.mux %sel, [[SPEC]], [[SHARED]] : i8
  // CHECK-NOT: comb.mux
  // CHECK: hw.output [[RESULT]], [[SPEC]],
  %spec = comb.shl %a, %b : i8
  %other = comb.mul %a, %b : i8
  %al = comb.extract %a from 0 : (i8) -> i4
  %bl = comb.extract %b from 0 : (i8) -> i4
  %narrow = comb.shl %al, %bl : i4
  %x = comb.mux %sel, %a, %c : i8
  %y = comb.mux %sel, %b, %c : i8
  %shared = comb.shl %x, %y : i8
  hw.output %shared, %spec, %narrow : i8, i8, i4
}

// Two shared multipliers that match each other when sel is true: only one
// takes its result from the other.
// CHECK-LABEL: hw.module @Mutual
hw.module @Mutual(in %a : i8, in %b : i8, in %c : i8, in %d : i8, in %e : i8,
                  in %sel : i1, out m : i8, out n : i8) {
  // CHECK: [[M:%.+]] = comb.mul {{%.+}}, {{%.+}} : i8
  // CHECK-NEXT: [[NEWM:%.+]] = comb.mux %sel, [[N:%.+]], [[M]] : i8
  // CHECK-NEXT: [[N]] = comb.mul {{%.+}}, {{%.+}} : i8
  // CHECK-NEXT: hw.output [[NEWM]], [[N]]
  %x = comb.mux %sel, %a, %c : i8
  %y = comb.mux %sel, %b, %d : i8
  %z = comb.mux %sel, %b, %e : i8
  %m = comb.mul %x, %y : i8
  %n = comb.mul %x, %z : i8
  hw.output %m, %n : i8, i8
}

// Multiplications of more than two operands are left alone.
// CHECK-LABEL: hw.module @Variadic
hw.module @Variadic(in %a : i8, in %b : i8, in %c : i8, in %sel : i1,
                    out m : i8, out n : i8) {
  // CHECK: [[M:%.+]] = comb.mul {{%.+}}, {{%.+}}, %c : i8
  // CHECK-NEXT: [[N:%.+]] = comb.mul %a, %b, %c : i8
  // CHECK-NEXT: hw.output [[M]], [[N]]
  %x = comb.mux %sel, %a, %c : i8
  %y = comb.mux %sel, %b, %c : i8
  %m = comb.mul %x, %y, %c : i8
  %n = comb.mul %a, %b, %c : i8
  hw.output %m, %n : i8, i8
}

// Where the whole result comes from the other operator, the shared one gets
// constant operands (one for a divisor); not when only the low bits do.
// ISOLATE-LABEL: hw.module @SameWidth
// ISOLATE: [[SPEC:%.+]] = comb.mul %a, %b : i8
// ISOLATE: [[SHARED:%.+]] = comb.mul [[X:%.+]], [[Y:%.+]] : i8
// ISOLATE: comb.mux %sel, [[SPEC]], [[SHARED]] : i8
// ISOLATE: [[X]] = comb.mux %sel, %c0_i8, {{%.+}} : i8
// ISOLATE: [[Y]] = comb.mux %sel, %c0_i8{{.*}}, {{%.+}} : i8
// The narrower multiplier gives the low 8 bits, a copy of the shared one
// with the operands of the case (sign extensions of %a and %b) the high 2.
// ISOLATE-LABEL: hw.module @Wider
// ISOLATE: [[SPEC:%.+]] = comb.mul %b, %a : i8
// ISOLATE: [[SHARED:%.+]] = comb.mul [[X:%.+]], [[Y:%.+]] : i10
// ISOLATE: [[AS:%.+]] = comb.extract %a from 7 : (i8) -> i1
// ISOLATE: [[AR:%.+]] = comb.replicate [[AS]] : (i1) -> i2
// ISOLATE: [[AE:%.+]] = comb.concat [[AR]], %a : i2, i8
// ISOLATE: [[BE:%.+]] = comb.concat {{%.+}}, %b : i2, i8
// ISOLATE: [[COPY:%.+]] = comb.mul [[AE]], [[BE]] : i10
// ISOLATE: [[HIGH:%.+]] = comb.extract [[COPY]] from 8 : (i10) -> i2
// ISOLATE: [[WHOLE:%.+]] = comb.concat [[HIGH]], [[SPEC]] : i2, i8
// ISOLATE: [[RESULT:%.+]] = comb.mux %nsel, [[WHOLE]], [[SHARED]] : i10
// ISOLATE: [[X]] = comb.mux %nsel, %c0_i10, {{%.+}} : i10
// ISOLATE: hw.output [[RESULT]], [[SPEC]]
// ISOLATE-LABEL: hw.module @TwoSelects
// ISOLATE-LABEL: hw.module @Division
// ISOLATE: comb.divu %a, %b : i8
// ISOLATE: [[DIV:%.+]] = comb.divu [[N:%.+]], [[D:%.+]] : i8
// ISOLATE: [[N]] = comb.mux %sel, %c0_i8, {{%.+}} : i8
// ISOLATE: [[D]] = comb.mux %sel, %c1_i8, {{%.+}} : i8
