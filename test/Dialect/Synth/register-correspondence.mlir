// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-register-correspondence))' | FileCheck %s
// REQUIRES: cadical

// Two registers with the same next state and initial value are one.
// CHECK-LABEL: hw.module @Duplicate
// CHECK:         %[[R:.+]] = seq.compreg %a, %clk initial
// CHECK-NOT:     seq.compreg
// CHECK:         synth.aig.and_inv %[[R]], not %[[R]]
hw.module @Duplicate(in %clk : !seq.clock, in %a : i1, out o : i1) {
  %init = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %r1 = seq.compreg %a, %clk initial %init : i1
  %r2 = seq.compreg %a, %clk initial %init : i1
  %0 = synth.aig.and_inv %r1, not %r2 : i1
  hw.output %0 : i1
}

// A register that always holds the inverse of another one.
// CHECK-LABEL: hw.module @Complement
// CHECK:         %[[R:.+]] = seq.compreg %a, %clk initial
// CHECK-NOT:     seq.compreg
// CHECK:         %[[N:.+]] = synth.aig.and_inv not %[[R]]
// CHECK:         hw.output %[[N]]
hw.module @Complement(in %clk : !seq.clock, in %a : i1, out o : i1) {
  %init0 = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %init1 = seq.initial() {
    %true = hw.constant true
    seq.yield %true : i1
  } : () -> !seq.immutable<i1>
  %na = synth.aig.and_inv not %a : i1
  %r1 = seq.compreg %a, %clk initial %init0 : i1
  %r2 = seq.compreg %na, %clk initial %init1 : i1
  hw.output %r2 : i1
}

// A register that stays at its initial value is a constant.
// CHECK-LABEL: hw.module @Constant
// CHECK-NOT:     seq.compreg
// CHECK:         %false = hw.constant false
// CHECK-NEXT:    hw.output %false
hw.module @Constant(in %clk : !seq.clock, in %a : i1, out o : i1) {
  %init = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %next = synth.aig.and_inv %r, %a : i1
  %r = seq.compreg %next, %clk initial %init : i1
  hw.output %r : i1
}

// Different next states, or different initial values: kept.
// CHECK-LABEL: hw.module @Different
// CHECK-COUNT-4: seq.compreg
hw.module @Different(in %clk : !seq.clock, in %a : i1, in %b : i1,
                     out o1 : i1, out o2 : i1, out o3 : i1, out o4 : i1) {
  %init0 = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %init1 = seq.initial() {
    %true = hw.constant true
    seq.yield %true : i1
  } : () -> !seq.immutable<i1>
  %r1 = seq.compreg %a, %clk initial %init0 : i1
  %r2 = seq.compreg %b, %clk initial %init0 : i1
  // Same next state as r3, but another initial value: they differ in the
  // first cycle.
  %r3 = seq.compreg %a, %clk initial %init1 : i1
  %r4 = seq.compreg %b, %clk initial %init1 : i1
  hw.output %r1, %r2, %r3, %r4 : i1, i1, i1, i1
}

// A bit of a wider register equal to another register: the wide register is
// split into one register per bit, and the equal bit is merged.
// CHECK-LABEL: hw.module @Split
// CHECK:         %[[R:.+]] = seq.compreg %a, %clk initial
// CHECK:         %[[HI:.+]] = seq.compreg {{.*}}, %clk initial
// CHECK-NOT:     seq.compreg
// CHECK:         %[[W:.+]] = comb.concat %[[HI]], %[[R]] : i1, i1
// CHECK:         hw.output %[[W]]
hw.module @Split(in %clk : !seq.clock, in %a : i1, in %b : i1, out o : i2) {
  %init = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %init2 = seq.initial() {
    %c0 = hw.constant 0 : i2
    seq.yield %c0 : i2
  } : () -> !seq.immutable<i2>
  %r = seq.compreg %a, %clk initial %init : i1
  %next = comb.concat %b, %a : i1, i1
  %w = seq.compreg %next, %clk initial %init2 : i2
  hw.output %w : i2
}

// Registers whose next states read themselves: r1 = r2 is inductive (if
// they are equal, their next states are).
// CHECK-LABEL: hw.module @Mutual
// CHECK-COUNT-1: seq.compreg
// CHECK-NOT:     seq.compreg
hw.module @Mutual(in %clk : !seq.clock, in %a : i1, out o : i1) {
  %init = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %n1 = synth.aig.and_inv %r1, %a : i1
  %n2 = synth.aig.and_inv %r2, %a : i1
  %x1 = synth.aig.and_inv not %n1, not %a : i1
  %x2 = synth.aig.and_inv not %n2, not %a : i1
  %r1 = seq.compreg %x1, %clk initial %init : i1
  %r2 = seq.compreg %x2, %clk initial %init : i1
  %0 = synth.aig.and_inv %r1, %r2 : i1
  hw.output %0 : i1
}

// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-register-correspondence{internal}))' | FileCheck %s --check-prefix=INT

// With internal, an and-gate equal to a register in all reachable states is
// replaced by the register: g = a & b is what r stores, one cycle late, but
// d = (a & b) delayed and r are equal; and an and-gate that is constant in
// reachable states (r & !r2 with r == r2) is a constant.
// CHECK-LABEL: hw.module @Internal
// INT-LABEL: hw.module @Internal
// INT:         %[[G:.+]] = synth.aig.and_inv %a, %b
// INT:         %[[R:.+]] = seq.compreg %[[G]], %clk initial
// INT-NOT:     seq.compreg
// INT:         hw.output %[[R]], %[[R]], %false
hw.module @Internal(in %clk : !seq.clock, in %a : i1, in %b : i1,
                    out x : i1, out y : i1, out z : i1) {
  %init = seq.initial() {
    %false = hw.constant false
    seq.yield %false : i1
  } : () -> !seq.immutable<i1>
  %g = synth.aig.and_inv %a, %b : i1
  %r = seq.compreg %g, %clk initial %init : i1
  %r2 = seq.compreg %g, %clk initial %init : i1
  // The same function as r, built from r2: equal to r in reachable states.
  %h = synth.aig.and_inv %r2, %r2 : i1
  %k = synth.aig.and_inv %r, not %r2 : i1
  hw.output %r, %h, %k : i1, i1, i1
}
