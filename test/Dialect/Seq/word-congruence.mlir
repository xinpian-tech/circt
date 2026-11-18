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

// A shadow register written through a different mux structure: p takes
// a when x, else (a if y else 0); q takes a. Both only when en = x | y.
// Their next states are numbered apart but equal whenever en holds, which a
// case split on x shows (with y from x | y by unit propagation).
// CHECK-LABEL: hw.module @shadow
// CHECK-COUNT-1: seq.compreg
// CHECK-NOT:     seq.compreg
hw.module @shadow(in %clk : !seq.clock, in %x : i1, in %y : i1, in %a : i8, out p : i8, out q : i8) {
  %init = seq.initial() {
    %c0 = hw.constant 0 : i8
    seq.yield %c0 : i8
  } : () -> !seq.immutable<i8>
  %c0 = hw.constant 0 : i8
  %en = comb.or %x, %y : i1
  %inner = comb.mux %y, %a, %c0 : i8
  %pa = comb.mux %x, %a, %inner : i8
  %pn = comb.mux %en, %pa, %p : i8
  %p = seq.compreg %pn, %clk initial %init : i8
  %qn = comb.mux %en, %a, %q : i8
  %q = seq.compreg %qn, %clk initial %init : i8
  hw.output %p, %q : i8, i8
}
