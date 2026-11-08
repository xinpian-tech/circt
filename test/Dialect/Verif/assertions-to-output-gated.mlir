// RUN: circt-opt --verif-assertions-to-output --split-input-file --verify-diagnostics %s | FileCheck %s

// An assertion on a gated clock is checked when the gate is enabled; its
// registers are on the gated clock (seq-clock-gate-to-enable gives them a
// clock enable).
// CHECK-LABEL: hw.module @Gated
hw.module @Gated(in %clk : i1, in %en : i1, in %a : i1, in %b : i1, out bad : i1) {
  %true = hw.constant true
  %c = seq.to_clock %clk
  %g = seq.clock_gate %c, %en
  %gclk = seq.from_clock %g
  // CHECK: [[G:%.+]] = seq.from_clock
  // CHECK: [[GC:%.+]] = seq.to_clock [[G]]
  // CHECK: [[R:%.+]] = seq.compreg %a, [[GC]] initial
  // CHECK: [[V1:%.+]] = comb.and bin [[R]], {{.*}} : i1
  // CHECK: [[G1:%.+]] = comb.and bin [[V1]], %en : i1
  %d = ltl.delay %true, 1, 0 : i1
  %s = ltl.concat %a, %d : i1, !ltl.sequence
  %p = ltl.implication %s, %b : !ltl.sequence, i1
  verif.clocked_assert %p, posedge %gclk label "gated" : !ltl.property
  // An assertion on the ungated clock is checked in every cycle.
  // CHECK: [[V2:%.+]] = comb.xor bin %a, {{%.+}}
  // CHECK: comb.or bin [[G1]], [[V2]] : i1
  verif.clocked_assert %a, posedge %clk label "ungated" : i1
  hw.output %true : i1
}

// -----

hw.module @TwoClocks(in %clk : i1, in %clk2 : i1, in %a : i1) {
  verif.clocked_assert %a, posedge %clk : i1
  // expected-error @below {{needs all assertions on the rising edge of the same clock (or gated versions of it)}}
  verif.clocked_assert %a, posedge %clk2 : i1
}

// -----

// A `disable iff` also applies in the cycles between those of the gated
// clock: `broken` (on the ungated clock) records a cycle without the enable
// since the last gated cycle.
// CHECK-LABEL: hw.module @GatedDisable
hw.module @GatedDisable(in %clk : i1, in %en : i1, in %rst : i1, in %a : i1, in %b : i1, out bad : i1) {
  %true = hw.constant true
  %c = seq.to_clock %clk
  %g = seq.clock_gate %c, %en
  %gclk = seq.from_clock %g
  %nrst = comb.xor %rst, %true : i1
  // CHECK: [[BROKEN:%.+]] = seq.compreg [[NEXT:%.+]], {{%.+}} initial
  // CHECK: [[NOW:%.+]] = comb.or bin [[BROKEN]], %rst : i1
  // CHECK: [[NEXT]] = comb.mux bin %en, {{%.+}}, [[NOW]] : i1
  // CHECK: [[NOTBROKEN:%.+]] = comb.xor bin [[BROKEN]], {{%.+}} : i1
  // CHECK: [[SINCE:%.+]] = comb.and bin {{%.+}}, [[NOTBROKEN]] : i1
  // CHECK: comb.and bin {{%.+}}, {{%.+}}, {{%.+}}, [[SINCE]], {{%.+}} : i1
  %d = ltl.delay %true, 1, 0 : i1
  %s = ltl.concat %a, %d : i1, !ltl.sequence
  %p = ltl.implication %s, %b : !ltl.sequence, i1
  verif.clocked_assert %p if %nrst, posedge %gclk : !ltl.property
  hw.output %true : i1
}
