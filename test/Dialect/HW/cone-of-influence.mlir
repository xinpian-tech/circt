// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(hw-cone-of-influence))' | FileCheck %s

// Registers that only feed each other are removed (canonicalize keeps the
// cycle); logic reaching the output or an assertion is kept.
// CHECK-LABEL: hw.module @Cycles
hw.module @Cycles(in %clk : !seq.clock, in %a : i1, in %b : i1, out o : i1) {
  // CHECK-NOT: %dead0
  // CHECK-NOT: %dead1
  %dead0 = seq.compreg %dead1, %clk : i1
  %dead1 = seq.compreg %dead0, %clk : i1
  // CHECK: %live = seq.compreg %a, %clk
  %live = seq.compreg %a, %clk : i1
  // CHECK: %checked = seq.compreg %b, %clk
  %checked = seq.compreg %b, %clk : i1
  // CHECK: verif.assert %checked
  verif.assert %checked : i1
  // CHECK: hw.output %live
  hw.output %live : i1
}

// A signal is kept with all its drives when it is read on a path to an
// output; unread signals go away.
// CHECK-LABEL: hw.module @Signals
hw.module @Signals(in %a : i1, in %b : i1, out o : i1) {
  %t = llhd.constant_time <0ns, 1d>
  // CHECK: [[S:%.+]] = llhd.sig %a
  // CHECK: llhd.drv [[S]], %b
  // CHECK: llhd.prb [[S]]
  %s = llhd.sig %a : <i1>
  llhd.drv %s, %b after %t : i1
  %p = llhd.prb %s : i1
  // CHECK-NOT: llhd.sig %b
  %u = llhd.sig %b : <i1>
  llhd.drv %u, %a after %t : i1
  hw.output %p : i1
}
