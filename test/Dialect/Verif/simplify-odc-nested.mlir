// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(verif-simplify-odc))' | FileCheck %s --check-prefixes=CHECK,ODC
// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(verif-simplify-odc{antecedent-only}))' | FileCheck %s --check-prefixes=CHECK,ANTE

// Don't-cares inside the property: the check is only observed where %sel = 1
// (mux operand, or second operand of an or).

// CHECK-LABEL: hw.module @MuxBranch
hw.module @MuxBranch(in %clk : i1, in %sel : i1, in %a : i8, in %b : i8,
                     in %c : i8) {
  %true = hw.constant true
  %x = comb.mux %sel, %a, %c : i8
  %r = comb.mul %x, %b : i8
  %spec = comb.mul %a, %b : i8
  %eq = comb.icmp eq %r, %spec : i8
  %p = comb.mux %sel, %eq, %true : i1
  // ODC: ltl.implication %true{{.*}}, %true
  // ANTE: [[P:%.+]] = comb.mux %sel, {{%.+}}, %true
  // ANTE: verif.clocked_assert [[P]]
  verif.clocked_assert %p, posedge %clk : i1
}

// CHECK-LABEL: hw.module @OrOperand
hw.module @OrOperand(in %clk : i1, in %sel : i1, in %a : i8, in %b : i8,
                     in %c : i8) {
  %true = hw.constant true
  %nsel = comb.xor %sel, %true : i1
  %x = comb.mux %sel, %a, %c : i8
  %eq = comb.icmp eq %x, %a : i8
  %p = comb.or %nsel, %eq : i1
  // ODC: ltl.implication %true{{.*}}, %true
  // ANTE: [[P:%.+]] = comb.or
  // ANTE: verif.clocked_assert [[P]]
  verif.clocked_assert %p, posedge %clk : i1
}

// An operand is not simplified with facts derived from itself: in x & x the
// second operand becomes 1 under the first, but the first stays x.
// CHECK-LABEL: hw.module @NoSelfReference
hw.module @NoSelfReference(in %clk : i1, in %x : i1, in %y : i1) {
  %m = comb.mux %x, %y, %x : i1
  %p = comb.and %x, %m : i1
  // ODC: [[P:%.+]] = comb.and %x, %y
  // ODC: ltl.implication %true{{.*}}, [[P]]
  verif.clocked_assert %p, posedge %clk : i1
}
