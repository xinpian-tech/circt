// RUN: circt-opt --pass-pipeline='builtin.module(hw.module(llhd-raise-clock-gates))' %s | FileCheck %s

// The latch-based clock gate (`always_latch if (!clk) q = en | te;
// assign gclk = clk & q;`) after import.
// CHECK-LABEL: hw.module @Gate
hw.module @Gate(in %clk : i1, in %en : i1, in %te : i1, in %d : i1, out q : i1) {
  %t = llhd.constant_time <0ns, 1d>
  %true = hw.constant true
  %q_en = llhd.sig %true : <i1>
  %p = llhd.prb %q_en : i1
  %nclk = comb.xor %clk, %true : i1
  %or = comb.or %en, %te : i1
  %v = comb.and %nclk, %or : i1
  llhd.drv %q_en, %v after %t if %nclk : i1
  %g = comb.and %clk, %p : i1
  %gclk = seq.to_clock %g
  // The enable is the latch input with the clock at 0.
  // CHECK: [[OR:%.+]] = comb.or %en, %te : i1
  // CHECK: [[F:%.+]] = hw.constant false
  // CHECK: [[NF:%.+]] = comb.xor [[F]], %true : i1
  // CHECK: [[EN:%.+]] = comb.and [[NF]], [[OR]] : i1
  // CHECK: [[CLK:%.+]] = seq.to_clock %clk
  // CHECK: [[GATE:%.+]] = seq.clock_gate [[CLK]], [[EN]]
  // CHECK: [[G:%.+]] = seq.from_clock [[GATE]]
  // CHECK: [[GCLK:%.+]] = seq.to_clock [[G]]
  // CHECK: seq.firreg %d clock [[GCLK]] : i1
  // CHECK-NOT: comb.and %clk
  %r = seq.firreg %d clock %gclk : i1
  hw.output %r : i1
}

// Not a clock gate: the signal is driven while the clock is high.
// CHECK-LABEL: hw.module @NotLow
// CHECK-NOT: seq.clock_gate
hw.module @NotLow(in %clk : i1, in %en : i1, in %d : i1, out q : i1) {
  %t = llhd.constant_time <0ns, 1d>
  %false = hw.constant false
  %s = llhd.sig %false : <i1>
  %p = llhd.prb %s : i1
  llhd.drv %s, %en after %t if %clk : i1
  %g = comb.and %clk, %p : i1
  %gclk = seq.to_clock %g
  %r = seq.firreg %d clock %gclk : i1
  hw.output %r : i1
}

// Not a clock gate: the signal has a second driver.
// CHECK-LABEL: hw.module @TwoDrivers
// CHECK-NOT: seq.clock_gate
hw.module @TwoDrivers(in %clk : i1, in %en : i1, in %d : i1, out q : i1) {
  %t = llhd.constant_time <0ns, 1d>
  %true = hw.constant true
  %s = llhd.sig %true : <i1>
  %p = llhd.prb %s : i1
  %nclk = comb.xor %clk, %true : i1
  llhd.drv %s, %en after %t if %nclk : i1
  llhd.drv %s, %d after %t : i1
  %g = comb.and %clk, %p : i1
  %gclk = seq.to_clock %g
  %r = seq.firreg %d clock %gclk : i1
  hw.output %r : i1
}
