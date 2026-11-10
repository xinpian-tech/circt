// RUN: circt-opt %s --pass-pipeline='builtin.module(verif-assertions-to-output)' | FileCheck %s
// RUN: circt-opt %s --pass-pipeline='builtin.module(verif-assertions-to-output{asserts=a_seq})' | FileCheck %s --check-prefix=SEL
// RUN: not circt-opt %s --pass-pipeline='builtin.module(verif-assertions-to-output{asserts=a_nope})' 2>&1 | FileCheck %s --check-prefix=ERR
// RUN: circt-opt %s --pass-pipeline='builtin.module(verif-assertions-to-output{constraints})' | FileCheck %s --check-prefix=CST

// ERR: no assertion labeled 'a_nope'

// CHECK-LABEL: hw.module @Props
// CHECK-SAME: out bad : i1)
hw.module @Props(in %clk : i1, in %en : i1, in %a : i1, in %b : i1,
                 in %c : i1, out o : i1) {
  // CHECK: [[CLK:%.+]] = seq.to_clock %clk

  // A boolean: violated where en & !a.
  // CHECK: [[NA:%.+]] = comb.xor bin %a, %{{.+}}
  // CHECK-NEXT: [[V1:%.+]] = comb.and bin [[NA]], %en
  verif.clocked_assert %a if %en, posedge %clk label "a_plain" : i1

  // a ##1 1 |-> ##2 c: checked when c is due, three cycles after a, with the
  // enable held in all four cycles. Registers start at 0.
  // CHECK: [[ZERO:%.+]] = seq.initial
  // CHECK: [[A1:%.+]] = seq.compreg %a, [[CLK]] initial [[ZERO]]
  // CHECK-NEXT: [[A2:%.+]] = seq.compreg [[A1]], [[CLK]] initial [[ZERO]]
  // CHECK-NEXT: [[A3:%.+]] = seq.compreg [[A2]], [[CLK]] initial [[ZERO]]
  // CHECK-NEXT: [[T1:%.+]] = seq.compreg %true, [[CLK]] initial [[ZERO]]
  // CHECK-NEXT: [[T2:%.+]] = seq.compreg [[T1]], [[CLK]] initial [[ZERO]]
  // CHECK: [[NC:%.+]] = comb.xor bin %c,
  // CHECK-NEXT: [[E1:%.+]] = seq.compreg %en, [[CLK]] initial [[ZERO]]
  // CHECK-NEXT: [[E2:%.+]] = seq.compreg [[E1]], [[CLK]] initial [[ZERO]]
  // CHECK-NEXT: [[E3:%.+]] = seq.compreg [[E2]], [[CLK]] initial [[ZERO]]
  // CHECK-NEXT: [[V2:%.+]] = comb.and bin [[A3]], [[T2]], [[NC]], %en, [[E1]], [[E2]], [[E3]]
  %t = hw.constant true
  %d1 = ltl.delay %t, 1, 0 : i1
  %s = ltl.concat %a, %d1 : i1, !ltl.sequence
  %d2 = ltl.delay %c, 2, 0 : i1
  %p = ltl.implication %s, %d2 : !ltl.sequence, !ltl.sequence
  verif.clocked_assert %p if %en, posedge %clk label "a_seq" : !ltl.property

  // An assumption: once violated, nothing counts any more.
  // CHECK: [[NB:%.+]] = comb.xor bin %b,
  // CHECK-NEXT: [[ANY:%.+]] = comb.or bin [[V1]], [[V2]]
  // CHECK: [[FAILED:%.+]] = seq.compreg [[NOW:%.+]], [[CLK]] initial [[ZERO]]
  // CHECK-NEXT: [[NOW]] = comb.or bin [[NB]], [[FAILED]]
  // CHECK-NEXT: %{{.+}} = hw.constant true
  // CHECK-NEXT: [[OK:%.+]] = comb.xor bin [[NOW]],
  // CHECK-NEXT: [[BAD:%.+]] = comb.and bin [[ANY]], [[OK]]
  // CHECK-NEXT: hw.output [[BAD]] : i1
  verif.clocked_assume %b, posedge %clk label "m_b" : i1
  hw.output %a : i1
}

// With constraints, the assumption is a verif.assume of the condition and
// the output is only the violation of the assertions.
// CST-LABEL: hw.module @Props
// CST:       [[ANY:%.+]] = comb.or bin
// CST:       verif.assume %{{.+}} : i1
// CST-NEXT:  hw.output [[ANY]] : i1

// Only the selected assertion is checked; the assumption stays.
// SEL-LABEL: hw.module @Props
// SEL-NOT: comb.xor bin %a,
// SEL: comb.and bin %{{.+}}, %{{.+}}, %{{.+}}, %en,
// SEL: hw.output

// A sequence holds where it matches: a ##1 b fails where a was 0 one cycle
// ago or b is 0 now.
// CHECK-LABEL: hw.module @Sequence
// CHECK: [[NA:%.+]] = comb.xor bin %a,
// CHECK: [[NA1:%.+]] = seq.compreg [[NA]]
// CHECK: [[NB:%.+]] = comb.xor bin %b,
// CHECK: [[V:%.+]] = comb.or bin [[NA1]], [[NB]]
// CHECK: hw.output [[V]]
hw.module @Sequence(in %clk : i1, in %a : i1, in %b : i1) {
  %d = ltl.delay %b, 1, 0 : i1
  %s = ltl.concat %a, %d : i1, !ltl.sequence
  verif.clocked_assert %s, posedge %clk : !ltl.sequence
}
