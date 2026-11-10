// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-ternary-constants))' | FileCheck %s

// r stays at its initial 0 (r' = r & a); s starts at 1 and s' = !r is always
// 1; t has an unknown initial value; u toggles. Only r and s are constants.
// CHECK-LABEL: hw.module @Constants
// CHECK-DAG:     %[[T:.+]] = seq.compreg %t_next, %clk :
// CHECK-DAG:     %[[U:.+]] = seq.compreg %[[NU:.+]], %clk initial
// CHECK-DAG:     %false = hw.constant false
// CHECK-DAG:     %true = hw.constant true
// CHECK:         hw.output %false, %true, %[[T]], %[[U]]
hw.module @Constants(in %clk : !seq.clock, in %a : i1, in %t_next : i1,
                     out r : i1, out s : i1, out t : i1, out u : i1) {
  %init0 = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %init1 = seq.initial() {
    %true = hw.constant true
    seq.yield %true : i1
  } : () -> !seq.immutable<i1>
  %rn = synth.aig.and_inv %r, %a : i1
  %r = seq.compreg %rn, %clk initial %init0 : i1
  %sn = synth.aig.and_inv not %r : i1
  %s = seq.compreg %sn, %clk initial %init1 : i1
  %t = seq.compreg %t_next, %clk : i1
  %un = synth.aig.and_inv not %u : i1
  %u = seq.compreg %un, %clk initial %init0 : i1
  hw.output %r, %s, %t, %u : i1, i1, i1, i1
}
