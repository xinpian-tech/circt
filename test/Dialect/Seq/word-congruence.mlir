// RUN: circt-opt %s --seq-word-congruence --canonicalize | FileCheck %s

// Two registers computing the same function of themselves (a counter and its
// copy, written differently) are merged; a register that differs is not.
// CHECK-LABEL: hw.module @m
// CHECK-COUNT-2: seq.compreg
// CHECK-NOT:     seq.compreg
hw.module @m(in %clk : !seq.clock, in %en : i1, out a : i8, out b : i8, out c : i8) {
  %init = seq.initial() {
    %c0 = hw.constant 0 : i8
    seq.yield %c0 : i8
  } : () -> !seq.immutable<i8>
  %one = hw.constant 1 : i8
  %two = hw.constant 2 : i8
  %a = seq.compreg %na, %clk initial %init : i8
  %sa = comb.add %a, %one : i8
  %na = comb.mux %en, %sa, %a : i8
  %b = seq.compreg %nb, %clk initial %init : i8
  %sb = comb.add %one, %b : i8
  %nb = comb.mux %en, %sb, %b : i8
  %c = seq.compreg %nc, %clk initial %init : i8
  %sc = comb.add %c, %two : i8
  %nc = comb.mux %en, %sc, %c : i8
  hw.output %a, %b, %c : i8, i8, i8
}
