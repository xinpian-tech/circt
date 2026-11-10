// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-temporal-decomposition))' | FileCheck %s --check-prefix=DEFAULT
// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-temporal-decomposition{force}))' | FileCheck %s
// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-temporal-decomposition{force bad-outputs}))' | FileCheck %s --check-prefix=BAD

// A two-stage reset sequence: s1 = 0, 1, 1, ...; s2 = 0, 0, 1, ... Both are
// 1 from cycle 2 on, so two cycles are skipped. d follows the input a: in
// cycle 2 it is the input of cycle 1, the new input a_pre1. The assertion
// !(s2 & d & !a) is true in the prefix (s2 is 0), so nothing of it is kept.
// Without `force` nothing is done: two registers go, but `first` and the
// logic that starts d are more.
// DEFAULT-LABEL: hw.module @Reset
// DEFAULT-NOT:     verif.target_shift
// DEFAULT:         %s1 = seq.compreg
// DEFAULT:         %s2 = seq.compreg
// CHECK-LABEL: hw.module @Reset
// CHECK-SAME:    in %a_pre1 : i1)
// CHECK-SAME:    attributes {verif.target_shift = 2 : i32}
// CHECK:         %[[D:d]] = seq.compreg %a, %clk : i1
// CHECK:         %[[FAILS:.+]] = synth.aig.and_inv %true{{.*}}, %[[DV:.+]], not %a : i1
// CHECK:         %[[P:.+]] = synth.aig.and_inv not %[[FAILS]] : i1
// CHECK:         verif.assert %[[P]] : i1
// CHECK:         %[[FIRST:prefix_first]] = seq.compreg %false, %clk initial %{{.+}} : i1
// CHECK:         %[[T1:.+]] = synth.aig.and_inv %[[FIRST]], %a_pre1 : i1
// CHECK:         %[[T2:.+]] = synth.aig.and_inv not %[[FIRST]], %[[D]] : i1
// CHECK:         %[[T3:.+]] = synth.aig.and_inv not %[[T1]], not %[[T2]] : i1
// CHECK:         %[[DV]] = synth.aig.and_inv not %[[T3]] : i1
hw.module @Reset(in %clk : !seq.clock, in %a : i1) {
  %init0 = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %true = hw.constant true
  %s1 = seq.compreg %true, %clk initial %init0 : i1
  %s2 = seq.compreg %s1, %clk initial %init0 : i1
  %d = seq.compreg %a, %clk initial %init0 : i1
  %bad = synth.aig.and_inv %s2, %d, not %a : i1
  %p = synth.aig.and_inv not %bad : i1
  verif.assert %p : i1
}

// The assertion can fail in the prefix: s = 0, 1, 1, ...; p = s | !a fails
// in cycle 0 with a = 1. The failure is kept under `first` over a_pre0; the
// constraint !b of the prefix guards it, and a prefix that breaks it ends
// the trace after the first cycle.
// CHECK-LABEL: hw.module @Prefix
// CHECK-SAME:    in %b_pre0 : i1, in %a_pre0 : i1)
// CHECK-SAME:    attributes {verif.target_shift = 1 : i32}
// CHECK:         verif.assert %[[H:.+]] : i1
// CHECK:         verif.assume %[[C:.+]] : i1
// CHECK:         %[[FIRST:prefix_first]] = seq.compreg %false, %clk initial %{{.+}} : i1
// CHECK:         %[[INV:.+]] = synth.aig.and_inv %[[FIRST]], %b_pre0 : i1
// CHECK:         %[[F0:.+]] = synth.aig.and_inv not %b_pre0, %a_pre0 : i1
// CHECK:         %[[FAIL:.+]] = synth.aig.and_inv %[[FIRST]], %[[F0]] : i1
// CHECK:         %[[H0:.+]] = synth.aig.and_inv not %{{.+}}, not %[[INV]] : i1
// CHECK:         %[[H]] = synth.aig.and_inv not %[[FAIL]], not %[[H0]] : i1
// CHECK:         %[[C0:.+]] = synth.aig.and_inv not %{{.+}}, not %[[INV]] : i1
// CHECK:         %[[C]] = synth.aig.and_inv not %[[C0]] : i1
// CHECK:         %[[ENDED:prefix_invalid]] = seq.compreg %[[INV]], %clk initial %{{.+}} : i1
// CHECK:         %[[NE:.+]] = synth.aig.and_inv not %[[ENDED]] : i1
// CHECK:         verif.assume %[[NE]] : i1
hw.module @Prefix(in %clk : !seq.clock, in %a : i1, in %b : i1) {
  %init0 = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %true = hw.constant true
  %s = seq.compreg %true, %clk initial %init0 : i1
  %bad = synth.aig.and_inv not %s, %a : i1
  %p = synth.aig.and_inv not %bad : i1
  verif.assert %p : i1
  %c = synth.aig.and_inv not %b : i1
  verif.assume %c : i1
}

// A module with outputs is left alone unless they are bad-state outputs.
// CHECK-LABEL: hw.module @Outputs
// CHECK:         %s = seq.compreg %true, %clk initial
// CHECK:         hw.output %{{.+}} : i1
// BAD-LABEL:   hw.module @Outputs
// BAD-SAME:      in %a_pre0 : i1, out bad : i1)
// BAD-SAME:      attributes {verif.target_shift = 1 : i32}
// BAD:           %[[NOW:.+]] = synth.aig.and_inv not %true{{.*}}, %a : i1
// BAD:           %[[FIRST:prefix_first]] = seq.compreg %false, %clk initial %{{.+}} : i1
// BAD:           %[[FAIL:.+]] = synth.aig.and_inv %[[FIRST]], %a_pre0 : i1
// BAD:           %[[OUT:.+]] = synth.aig.and_inv not %[[FAIL]], not %[[NOW]] : i1
// BAD:           %[[O:.+]] = synth.aig.and_inv not %[[OUT]] : i1
// BAD:           hw.output %[[O]] : i1
hw.module @Outputs(in %clk : !seq.clock, in %a : i1, out bad : i1) {
  %init0 = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %true = hw.constant true
  %s = seq.compreg %true, %clk initial %init0 : i1
  %bad = synth.aig.and_inv not %s, %a : i1
  hw.output %bad : i1
}

// A register with reset is not modeled: nothing is done.
// CHECK-LABEL: hw.module @Unmodeled
// CHECK-NOT:     verif.target_shift
// CHECK:         %s = seq.compreg %true, %clk initial
hw.module @Unmodeled(in %clk : !seq.clock, in %rst : i1, in %a : i1) {
  %init0 = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %true = hw.constant true
  %false = hw.constant false
  %s = seq.compreg %true, %clk initial %init0 : i1
  %r = seq.compreg %a, %clk reset %rst, %false : i1
  %p = synth.aig.and_inv %s, %r : i1
  verif.assert %p : i1
}

// Five registers are 1 from cycle 1 on and the assertion holds in cycle 0:
// skipping it only costs `first`, so it is done without `force`.
// DEFAULT-LABEL: hw.module @Wide
// DEFAULT-SAME:    attributes {verif.target_shift = 1 : i32}
// DEFAULT-NOT:     seq.compreg
// DEFAULT:         %[[F:.+]] = synth.aig.and_inv %true{{.*}}, %true{{.*}}, %true{{.*}}, %true{{.*}}, %true{{.*}}, %a : i1
// DEFAULT:         %[[P:.+]] = synth.aig.and_inv not %[[F]] : i1
// DEFAULT:         verif.assert %[[P]] : i1
hw.module @Wide(in %clk : !seq.clock, in %a : i1) {
  %init0 = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %true = hw.constant true
  %s1 = seq.compreg %true, %clk initial %init0 : i1
  %s2 = seq.compreg %true, %clk initial %init0 : i1
  %s3 = seq.compreg %true, %clk initial %init0 : i1
  %s4 = seq.compreg %true, %clk initial %init0 : i1
  %s5 = seq.compreg %true, %clk initial %init0 : i1
  %bad = synth.aig.and_inv %s1, %s2, %s3, %s4, %s5, %a : i1
  %p = synth.aig.and_inv not %bad : i1
  verif.assert %p : i1
}
