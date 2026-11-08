// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(verif-simplify-odc))' --verify-diagnostics | FileCheck %s
// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(verif-simplify-odc{antecedent-only}))' --verify-diagnostics | FileCheck %s

// The antecedent picks the mux input, so both sides of the comparison are the
// same product. The design (the output) keeps the original mux.
// CHECK-LABEL: hw.module @MuxOperand
hw.module @MuxOperand(in %clk : i1, in %op : i1, in %a : i8, in %b : i8,
                      in %c : i8, out r : i8) {
  // CHECK: [[X:%.+]] = comb.mux %op, %a, %c
  // CHECK: [[R:%.+]] = comb.mul [[X]], %b
  // CHECK-NOT: comb.mul
  %x = comb.mux %op, %a, %c {sv.namehint = "x"} : i8
  %r = comb.mul %x, %b {sv.namehint = "r"} : i8
  %spec = comb.mul %a, %b {sv.namehint = "spec"} : i8
  %eq = comb.icmp eq %r, %spec : i8
  // CHECK: [[TRUE:%.+]] = hw.constant true
  // CHECK: [[P:%.+]] = ltl.implication %op, [[TRUE]]
  // CHECK: verif.clocked_assert [[P]]
  %p = ltl.implication %op, %eq : i1, i1
  // expected-remark @below {{property holds under its don't-cares}}
  verif.clocked_assert %p, posedge %clk : !ltl.property
  // CHECK: hw.output [[R]]
  hw.output %r : i8
}

// A wrong implementation is not proved: the other mux input remains.
// CHECK-LABEL: hw.module @WrongOperand
hw.module @WrongOperand(in %clk : i1, in %op : i1, in %a : i8, in %b : i8,
                        in %c : i8) {
  %x = comb.mux %op, %c, %a : i8
  %r = comb.mul %x, %b : i8
  %spec = comb.mul %a, %b : i8
  %eq = comb.icmp eq %r, %spec : i8
  // CHECK: [[R:%.+]] = comb.mul %c, %b
  // CHECK: [[EQ:%.+]] = comb.icmp eq [[R]], {{%.+}}
  // CHECK: ltl.implication %op, [[EQ]]
  %p = ltl.implication %op, %eq : i1, i1
  verif.clocked_assert %p, posedge %clk : !ltl.property
}

// Facts from conjunctions, negations, `x == C` comparisons and the enable.
// CHECK-LABEL: hw.module @Facts
hw.module @Facts(in %clk : i1, in %en : i1, in %state : i2, in %kill : i1,
                 in %a : i8, in %b : i8) {
  %c2 = hw.constant 2 : i2
  %true = hw.constant true
  %isTwo = comb.icmp eq %state, %c2 : i2
  %notKill = comb.xor %kill, %true : i1
  %ante = comb.and %isTwo, %notKill : i1
  %s = comb.icmp eq %state, %c2 : i2
  %x = comb.mux %s, %a, %b : i8
  %y = comb.mux %kill, %b, %x : i8
  %z = comb.mux %en, %y, %b : i8
  %eq = comb.icmp eq %z, %a : i8
  // CHECK: ltl.implication {{%.+}}, %true
  %p = ltl.implication %ante, %eq : i1, i1
  // expected-remark @below {{property holds under its don't-cares (label 'a_facts')}}
  verif.clocked_assert %p if %en, posedge %clk label "a_facts" : !ltl.property
}

// `x + acc == x` becomes `acc == 0`.
// CHECK-LABEL: hw.module @CancelAdd
hw.module @CancelAdd(in %clk : i1, in %op : i1, in %a : i8, in %b : i8,
                     in %acc : i8) {
  %x = comb.mux %op, %a, %b : i8
  %sum = comb.add %x, %acc : i8
  %eq = comb.icmp eq %sum, %a : i8
  // CHECK: [[ZERO:%.+]] = hw.constant 0 : i8
  // CHECK: [[EQ:%.+]] = comb.icmp eq %acc, [[ZERO]]
  // CHECK: ltl.implication %op, [[EQ]]
  %p = ltl.implication %op, %eq : i1, i1
  verif.clocked_assert %p, posedge %clk : !ltl.property
}

// The antecedent does not decide %m, but the consequent holds for %m = 1:
// the assertion becomes `(st && !m) |-> c[m := 0]`.
// CHECK-LABEL: hw.module @Split
hw.module @Split(in %clk : i1, in %st : i1, in %m : i1, in %a : i8,
                 in %b : i8, in %c : i8) {
  %x = comb.mux %st, %a, %c : i8
  %y = comb.mux %m, %x, %b : i8
  %eq = comb.icmp eq %y, %a : i8
  // CHECK: [[EQ:%.+]] = comb.icmp eq %b, %a
  // CHECK: [[NOTM:%.+]] = comb.xor %m, %true
  // CHECK: [[ANTE:%.+]] = comb.and %st, [[NOTM]]
  // CHECK: ltl.implication [[ANTE]], [[EQ]]
  %p = ltl.implication %st, %eq : i1, i1
  verif.clocked_assert %p, posedge %clk : !ltl.property
}

// Properties that are not implications of booleans are left alone.
// CHECK-LABEL: hw.module @Sequence
hw.module @Sequence(in %clk : i1, in %a : i1, in %b : i1) {
  // CHECK: [[D:%.+]] = ltl.delay %a, 1
  // CHECK: ltl.implication [[D]], %b
  %d = ltl.delay %a, 1, 0 : i1
  %p = ltl.implication %d, %b : !ltl.sequence, i1
  verif.clocked_assert %p, posedge %clk : !ltl.property
}

// Assumptions are simplified the same way, but one that folds to true is not
// reported as proved.
// CHECK-LABEL: hw.module @Assume
hw.module @Assume(in %clk : i1, in %op : i1, in %a : i8, in %b : i8,
                  in %c : i8) {
  %x = comb.mux %op, %a, %c : i8
  %eq = comb.icmp eq %x, %a : i8
  // CHECK: ltl.implication %op, %true
  %p = ltl.implication %op, %eq : i1, i1
  verif.clocked_assume %p, posedge %clk : !ltl.property
}

// Properties nested in other ops are left alone.
// CHECK-LABEL: hw.module @Nested
hw.module @Nested(in %clk : i1, in %op : i1, in %a : i8, in %c : i8) {
  %x = comb.mux %op, %a, %c : i8
  %eq = comb.icmp eq %x, %a : i8
  // CHECK: [[EQ:%.+]] = comb.icmp eq
  // CHECK: sv.always
  // CHECK-NEXT: ltl.implication %op, [[EQ]]
  sv.always posedge %clk {
    %p = ltl.implication %op, %eq : i1, i1
    verif.assert %p : !ltl.property
  }
}
