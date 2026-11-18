// REQUIRES: bitwuzla
// RUN: circt-opt %s --seq-word-correspondence | FileCheck %s

// a and b hold the same word; a + a and b << 1 are equal; c stays 0; the low
// bit of a + a is 0.
// CHECK-LABEL: hw.module @m
// CHECK-DAG:   %a = seq.compreg [[NA:%.+]], %clk
// CHECK-DAG:   [[NA]] = comb.mux %en, %d, %a
// CHECK-DAG:   [[S:%.+]] = comb.add %a, %a
// CHECK-DAG:   [[HI:%.+]] = comb.extract [[S]] from 1 : (i8) -> i7
// CHECK-DAG:   [[LOW:%.+]] = comb.concat [[HI]], %false
// CHECK-DAG:   %c0_i8 = hw.constant 0 : i8
// CHECK:       hw.output [[LOW]], [[LOW]], %c0_i8
hw.module @m(in %clk : !seq.clock, in %d : i8, in %en : i1, out o : i8, out p : i8, out q : i8) {
  %i0 = seq.initial() {
    %c0 = hw.constant 0 : i8
    seq.yield %c0 : i8
  } : () -> !seq.immutable<i8>
  %a = seq.compreg %na, %clk initial %i0 : i8
  %b = seq.compreg %nb, %clk initial %i0 : i8
  %na = comb.mux %en, %d, %a : i8
  %nb = comb.mux %en, %d, %b : i8
  %s1 = comb.add %a, %a : i8
  %c1 = hw.constant 1 : i8
  %s2 = comb.shl %b, %c1 : i8
  %c = seq.compreg %nc, %clk initial %i0 : i8
  %nc = comb.and %c, %d : i8
  hw.output %s1, %s2, %c : i8, i8, i8
}
