// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-register-correspondence{internal}))' --split-input-file | FileCheck %s
// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-register-correspondence{internal constraints}))' --split-input-file --verify-diagnostics | FileCheck %s --check-prefix=CONS
// REQUIRES: cadical

// The constraint is !(x & y). Under it g = (x & y) & a is 0, and so is r,
// which stores g; k = x & y is in the cone of the constraint and is not
// merged with 0 (the constraint would then always hold).
// CHECK-LABEL: hw.module @Constrained
// CHECK:         seq.compreg
// CHECK-NOT:     hw.output %false, %false
// CONS-LABEL:  hw.module @Constrained
// CONS-NOT:      seq.compreg
// CONS-DAG:      %[[K:.+]] = synth.aig.and_inv %x, %y
// CONS-DAG:      %[[NK:.+]] = synth.aig.and_inv not %[[K]]
// CONS-DAG:      verif.assume %[[NK]]
// CONS:          hw.output %false, %false
hw.module @Constrained(in %clk : !seq.clock, in %a : i1, in %x : i1,
                       in %y : i1, out o1 : i1, out o2 : i1) {
  %init = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %k = synth.aig.and_inv %x, %y : i1
  %nk = synth.aig.and_inv not %k : i1
  verif.assume %nk : i1
  %g = synth.aig.and_inv %k, %a : i1
  %r = seq.compreg %g, %clk initial %init : i1
  hw.output %g, %r : i1, i1
}

// -----

// Without constraints nothing changes.
// CONS-LABEL:  hw.module @NoConstraints
// CONS:          seq.compreg
hw.module @NoConstraints(in %clk : !seq.clock, in %a : i1, out o : i1) {
  %init = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %r = seq.compreg %a, %clk initial %init : i1
  hw.output %r : i1
}

// -----

// The speculatively reduced proof does not support constraints.
// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-register-correspondence{internal constraints chunk=0}))' --split-input-file 2>&1 | FileCheck %s --check-prefix=SPEC
// SPEC: warning: constraints need chunk > 0; not used
hw.module @Spec(in %clk : !seq.clock, in %a : i1, in %x : i1, out o : i1) {
  %init = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %nx = synth.aig.and_inv not %x : i1
  verif.assume %nx : i1
  %g = synth.aig.and_inv %x, %a : i1
  %r = seq.compreg %g, %clk initial %init : i1
  hw.output %r : i1
}
