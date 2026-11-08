// RUN: circt-opt --pass-pipeline='builtin.module(hw.module(seq-clock-gate-to-enable))' %s | FileCheck %s

// CHECK-LABEL: hw.module @Regs
hw.module @Regs(in %clk : !seq.clock, in %en : i1, in %te : i1, in %rst : i1, in %d : i8, out a : i8, out b : i8, out c : i8, out e : i8) {
  %c0 = hw.constant 0 : i8
  %g = seq.clock_gate %clk, %en
  // CHECK: [[MA:%.+]] = comb.mux bin %en, %d, %a : i8
  // CHECK: [[RA:%.+]] = comb.and bin %rst, %en : i1
  // CHECK: %a = seq.firreg [[MA]] clock %clk reset sync [[RA]], %c0_i8 : i8
  %a = seq.firreg %d clock %g reset sync %rst, %c0 : i8
  // An asynchronous reset is not gated.
  // CHECK: [[MB:%.+]] = comb.mux bin %en, %d, %b : i8
  // CHECK: %b = seq.firreg [[MB]] clock %clk reset async %rst, %c0_i8 : i8
  %b = seq.firreg %d clock %g reset async %rst, %c0 : i8
  // A chain of gates, with a test enable.
  // CHECK: [[OR:%.+]] = comb.or bin %en, %te : i1
  // CHECK: [[EN2:%.+]] = comb.and bin [[OR]], %en : i1
  // CHECK: [[MC:%.+]] = comb.mux bin [[EN2]], %d, %c : i8
  // CHECK: %c = seq.compreg [[MC]], %clk : i8
  %g2 = seq.clock_gate %g, %en, %te
  %c = seq.compreg %d, %g2 : i8
  // CHECK: [[CE:%.+]] = comb.and bin %te, %en : i1
  // CHECK: %e = seq.compreg.ce %d, %clk, [[CE]] : i8
  %e = seq.compreg.ce %d, %g, %te : i8
  // CHECK-NOT: seq.clock_gate
  hw.output %a, %b, %c, %e : i8, i8, i8, i8
}

// Other users keep the gated clock.
// CHECK-LABEL: hw.module @OtherUser
// CHECK: [[G:%.+]] = seq.clock_gate %clk, %en
// CHECK: hw.output [[G]]
hw.module @OtherUser(in %clk : !seq.clock, in %en : i1, out g : !seq.clock) {
  %g = seq.clock_gate %clk, %en
  hw.output %g : !seq.clock
}
