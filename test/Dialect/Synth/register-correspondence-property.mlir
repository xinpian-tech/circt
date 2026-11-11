// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-register-correspondence))' --split-input-file | FileCheck %s
// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-register-correspondence{property-directed bad-outputs}))' --split-input-file --verify-diagnostics | FileCheck %s --check-prefix=PD
// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-register-correspondence{property-directed bad-outputs chunk=0}))' --split-input-file --verify-diagnostics | FileCheck %s --check-prefix=PD
// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-register-correspondence{property-directed bad-outputs conjunction}))' --split-input-file | FileCheck %s --check-prefix=CONJ
// REQUIRES: cadical

// p and q store a, except that q stores !a once the sticky error e is set.
// They differ only after the bad output e is 1, so they are merged only when
// the step may assume that the property (e == 0) held in the cycle before.
// CHECK-LABEL: hw.module @AfterFailure
// CHECK-COUNT-3: seq.compreg
// PD-LABEL:    hw.module @AfterFailure
// PD-COUNT-2:    seq.compreg
// PD-NOT:        seq.compreg
// PD:            hw.output
hw.module @AfterFailure(in %clk : !seq.clock, in %a : i1, in %x : i1,
                        out bad : i1) {
  %init = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %ne = synth.aig.and_inv not %e, not %x : i1
  %en = synth.aig.and_inv not %ne : i1
  %e = seq.compreg %en, %clk initial %init : i1
  %p = seq.compreg %a, %clk initial %init : i1
  %t1 = synth.aig.and_inv %e, not %a : i1
  %t2 = synth.aig.and_inv not %e, %a : i1
  %nq = synth.aig.and_inv not %t1, not %t2 : i1
  %qn = synth.aig.and_inv not %nq : i1
  %q = seq.compreg %qn, %clk initial %init : i1
  %d = synth.aig.and_inv %p, not %q : i1
  %b = synth.aig.and_inv not %e, not %d : i1
  %bad = synth.aig.and_inv not %b : i1
  hw.output %bad : i1
}

// -----

// The property can also be a verif.assert (without bad-outputs it is the
// only one; with bad-outputs there is no output, so it still is).
// PD-LABEL:    hw.module @Assert
// PD-COUNT-2:    seq.compreg
// PD-NOT:        seq.compreg
// PD:            verif.assert
hw.module @Assert(in %clk : !seq.clock, in %a : i1, in %x : i1) {
  %init = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %ne = synth.aig.and_inv not %e, not %x : i1
  %en = synth.aig.and_inv not %ne : i1
  %e = seq.compreg %en, %clk initial %init : i1
  %p = seq.compreg %a, %clk initial %init : i1
  %t1 = synth.aig.and_inv %e, not %a : i1
  %t2 = synth.aig.and_inv not %e, %a : i1
  %nq = synth.aig.and_inv not %t1, not %t2 : i1
  %qn = synth.aig.and_inv not %nq : i1
  %q = seq.compreg %qn, %clk initial %init : i1
  %d = synth.aig.and_inv %p, not %q : i1
  %ok = synth.aig.and_inv not %e, not %d : i1
  verif.assert %ok : i1
}

// -----

// Two properties: e could fail first and hide a later failure of the other
// one, so nothing is assumed and p, q stay apart; with `conjunction` both
// are assumed in the step and p, q are merged.
// PD-LABEL:    hw.module @TwoProperties
// PD-COUNT-3:    seq.compreg
// CONJ-LABEL:  hw.module @TwoProperties
// CONJ-COUNT-2:  seq.compreg
// CONJ-NOT:      seq.compreg
// CONJ:          hw.output
// expected-warning @below {{property-directed needs exactly one i1 property (or conjunction), found 2; not assumed}}
hw.module @TwoProperties(in %clk : !seq.clock, in %a : i1, in %x : i1,
                         out bad : i1, out bad2 : i1) {
  %init = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %ne = synth.aig.and_inv not %e, not %x : i1
  %en = synth.aig.and_inv not %ne : i1
  %e = seq.compreg %en, %clk initial %init : i1
  %p = seq.compreg %a, %clk initial %init : i1
  %t1 = synth.aig.and_inv %e, not %a : i1
  %t2 = synth.aig.and_inv not %e, %a : i1
  %nq = synth.aig.and_inv not %t1, not %t2 : i1
  %qn = synth.aig.and_inv not %nq : i1
  %q = seq.compreg %qn, %clk initial %init : i1
  %d = synth.aig.and_inv %p, not %q : i1
  hw.output %e, %d : i1, i1
}
