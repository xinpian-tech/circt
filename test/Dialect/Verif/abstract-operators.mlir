// RUN: circt-opt %s --verif-abstract-operators | FileCheck %s
// RUN: circt-opt %s --verif-abstract-operators=history=false | FileCheck %s --check-prefix=NOHIST

// Two multipliers of different widths: results become inputs, compared on
// the low bits, in the same cycle and against the previous one.
// CHECK-LABEL: hw.module @Two
// CHECK-SAME: in %abstract_mul_0 : i16, in %abstract_mul_1 : i12
// NOHIST-LABEL: hw.module @Two
hw.module @Two(in %clk : !seq.clock, in %a : i16, in %b : i16, in %c : i12,
               in %d : i12, out o : i16, out p : i12) {
  // CHECK-NOT: comb.mul
  // CHECK: %q = seq.compreg %abstract_mul_0, %clk : i16
  // CHECK: %abstract_mul_0_lhs = seq.compreg %a, %clk : i16
  // CHECK: %abstract_mul_0_rhs = seq.compreg %b, %clk : i16
  // CHECK: %abstract_mul_0_result = seq.compreg %abstract_mul_0, %clk : i16
  // CHECK: %abstract_mul_1_lhs = seq.compreg %c, %clk : i12
  // Same cycle, low 12 bits.
  // CHECK: [[R0:%.+]] = comb.extract %abstract_mul_0 from 0 : (i16) -> i12
  // CHECK: [[RES:%.+]] = comb.icmp bin eq [[R0]], %abstract_mul_1 : i12
  // CHECK: [[A0:%.+]] = comb.extract %a from 0 : (i16) -> i12
  // CHECK: [[EA:%.+]] = comb.icmp bin eq [[A0]], %c : i12
  // CHECK: [[B0:%.+]] = comb.extract %b from 0 : (i16) -> i12
  // CHECK: [[EB:%.+]] = comb.icmp bin eq [[B0]], %d : i12
  // CHECK: [[PRE:%.+]] = comb.and bin [[EA]], [[EB]] : i1
  // CHECK: [[NPRE:%.+]] = comb.xor bin [[PRE]], %true
  // CHECK: [[IMP:%.+]] = comb.or bin [[NPRE]], [[RES]] : i1
  // CHECK: verif.assume [[IMP]] : i1
  // Previous cycle.
  // CHECK: comb.icmp bin eq %abstract_mul_0, %abstract_mul_0_result : i16
  // CHECK: hw.output %abstract_mul_0, %abstract_mul_1
  // NOHIST-NOT: abstract_mul_0_lhs
  // NOHIST: verif.assume
  // NOHIST: verif.assume
  // NOHIST-NEXT: hw.output
  %m = comb.mul %a, %b : i16
  %n = comb.mul %c, %d : i12
  %q = seq.compreg %m, %clk : i16
  hw.output %m, %n : i16, i12
}

// Range: operands extended from 4 bits give a result that fits in 8 bits.
// CHECK-LABEL: hw.module @Range
hw.module @Range(in %a : i4, in %b : i4, out o : i12) {
  // CHECK: [[AH:%.+]] = comb.extract [[A:%.+]] from 4 : (i12) -> i8
  // CHECK: [[AZ:%.+]] = comb.icmp bin eq [[AH]], %c0_i8
  // CHECK: [[BH:%.+]] = comb.extract [[B:%.+]] from 4 : (i12) -> i8
  // CHECK: [[BZ:%.+]] = comb.icmp bin eq [[BH]], %c0_i8
  // CHECK: [[PRE:%.+]] = comb.and bin [[AZ]], [[BZ]] : i1
  // CHECK: [[RH:%.+]] = comb.extract %abstract_mul_0 from 8 : (i12) -> i4
  // CHECK: [[RZ:%.+]] = comb.icmp bin eq [[RH]], %c0_i4
  // CHECK: [[NPRE:%.+]] = comb.xor bin [[PRE]], %true
  // CHECK: [[IMP:%.+]] = comb.or bin [[NPRE]], [[RZ]] : i1
  // CHECK: verif.assume [[IMP]] : i1
  %c0 = hw.constant 0 : i8
  %az = comb.concat %c0, %a : i8, i4
  %bz = comb.concat %c0, %b : i8, i4
  %m = comb.mul %az, %bz : i12
  hw.output %m : i12
}

// Constant operands and narrow multipliers stay.
// CHECK-LABEL: hw.module @Kept
hw.module @Kept(in %a : i16, in %b : i4, in %c : i4, out o : i16, out p : i4) {
  // CHECK: comb.mul %a, %c3_i16 : i16
  // CHECK: comb.mul %b, %c : i4
  // CHECK-NOT: verif.assume
  %c3_i16 = hw.constant 3 : i16
  %m = comb.mul %a, %c3_i16 : i16
  %n = comb.mul %b, %c : i4
  hw.output %m, %n : i16, i4
}
