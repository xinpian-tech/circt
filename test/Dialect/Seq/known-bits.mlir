// RUN: circt-opt %s --seq-known-bits --canonicalize | FileCheck %s

// c starts at 0 and only keeps bits of itself: always 0. The counter's low
// bit is set and its top nibble stays 0: n starts at 1 and adds 2 under a
// mask of the low nibble.
// CHECK-LABEL: hw.module @m
// CHECK-DAG:     %c0_i8 = hw.constant 0 : i8
// CHECK-DAG:     [[N:%.+]] = comb.extract %n from 1 : (i8) -> i3
// CHECK-DAG:     [[NC:%.+]] = comb.concat %c0_i4, [[N]], %true : i4, i3, i1
// CHECK:         hw.output %c0_i8, [[NC]]
hw.module @m(in %clk : !seq.clock, in %d : i8, out q : i8, out r : i8) {
  %i0 = seq.initial() {
    %c0 = hw.constant 0 : i8
    seq.yield %c0 : i8
  } : () -> !seq.immutable<i8>
  %i1 = seq.initial() {
    %c1 = hw.constant 1 : i8
    seq.yield %c1 : i8
  } : () -> !seq.immutable<i8>
  %c = seq.compreg %nc, %clk initial %i0 : i8
  %nc = comb.and %c, %d : i8
  %two = hw.constant 2 : i8
  %mask = hw.constant 15 : i8
  %n = seq.compreg %nn, %clk initial %i1 : i8
  %sum = comb.add %n, %two : i8
  %nn = comb.and %sum, %mask : i8
  hw.output %c, %n : i8, i8
}
