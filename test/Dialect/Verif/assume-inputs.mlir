// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(verif-split-assumes))' | FileCheck %s --check-prefix=SPLIT
// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(verif-split-assumes,verif-substitute-assumed-inputs))' | FileCheck %s

// An assumed conjunction becomes one assumption per operand; an inverted
// operand gets an inverter; an assumed true is removed.
// SPLIT-LABEL: hw.module @Split
// SPLIT-DAG:     verif.assume %a : i1
// SPLIT-DAG:     [[NB:%.+]] = synth.aig.and_inv not %b : i1
// SPLIT-DAG:     verif.assume [[NB]] : i1
// SPLIT-DAG:     verif.assume %c : i1
// SPLIT-NOT:     verif.assume
// SPLIT:         hw.output
hw.module @Split(in %a : i1, in %b : i1, in %c : i1) {
  %0 = synth.aig.and_inv %a, not %b : i1
  %1 = comb.and %0, %c : i1
  verif.assume %1 : i1
  %true = hw.constant true
  verif.assume %true : i1
  hw.output
}

// An assumed input is a constant, except in its assumption.
// CHECK-LABEL: hw.module @Constant
// CHECK-DAG:     verif.assume %a : i1
// CHECK-DAG:     [[NB:%.+]] = synth.aig.and_inv not %b
// CHECK-DAG:     verif.assume [[NB]] : i1
// CHECK-DAG:     [[T:%.+]] = hw.constant true
// CHECK-DAG:     [[F:%.+]] = hw.constant false
// CHECK:         hw.output [[T]], [[F]] : i1, i1
hw.module @Constant(in %a : i1, in %b : i1, out x : i1, out y : i1) {
  verif.assume %a : i1
  %nb = synth.aig.and_inv not %b : i1
  verif.assume %nb : i1
  hw.output %a, %b : i1, i1
}

// An input assumed equal to a function of other signals (an XNOR, split
// into two implications) is replaced by it, except in the assumptions.
// CHECK-LABEL: hw.module @Equal
// CHECK:         %[[F:.+]] = synth.aig.and_inv %b, %c : i1
// CHECK:         %[[I1:.+]] = synth.aig.and_inv %a, not %[[F]] : i1
// CHECK:         %[[I2:.+]] = synth.aig.and_inv not %a, %[[F]] : i1
// CHECK:         hw.output %[[F]] : i1
hw.module @Equal(in %a : i1, in %b : i1, in %c : i1, out x : i1) {
  %f = synth.aig.and_inv %b, %c : i1
  %i1 = synth.aig.and_inv %a, not %f : i1
  %i2 = synth.aig.and_inv not %a, %f : i1
  %xnor = synth.aig.and_inv not %i1, not %i2 : i1
  verif.assume %xnor : i1
  hw.output %a : i1
}

// A bit of a wider input, assumed equal with comb.icmp.
// CHECK-LABEL: hw.module @Bit
// CHECK:         %[[A0:.+]] = comb.extract %a from 0 : (i2) -> i1
// CHECK:         %[[EQ:.+]] = comb.icmp eq %[[A0]], %b : i1
// CHECK:         verif.assume %[[EQ]] : i1
// CHECK:         hw.output %b : i1
hw.module @Bit(in %a : i2, in %b : i1, out x : i1) {
  %a0 = comb.extract %a from 0 : (i2) -> i1
  %eq = comb.icmp eq %a0, %b : i1
  verif.assume %eq : i1
  hw.output %a0 : i1
}

// Not replaced: only one implication, or the value depends on the input.
// CHECK-LABEL: hw.module @Kept
// CHECK:         hw.output %a, %b : i1, i1
hw.module @Kept(in %a : i1, in %b : i1, in %c : i1, out x : i1, out y : i1) {
  // a -> c only.
  %i1 = synth.aig.and_inv %a, not %c : i1
  %n1 = synth.aig.and_inv not %i1 : i1
  verif.assume %n1 : i1
  // b == (b & c): depends on b itself.
  %g = synth.aig.and_inv %b, %c : i1
  %eq = comb.icmp eq %b, %g : i1
  verif.assume %eq : i1
  hw.output %a, %b : i1, i1
}
