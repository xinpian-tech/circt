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

// With context, a mux arm is evaluated under its condition: r is written x
// only when x == 4, so it is 0 or 4: only bit 2 is unknown.
// RUN: circt-opt %s --seq-known-bits=context --canonicalize | FileCheck %s --check-prefix=CTX
// CTX-LABEL: hw.module @ctx
// CTX:         [[R:%.+]] = comb.extract %r from 2 : (i4) -> i1
// CTX:         comb.concat %false, [[R]], %c0_i2
hw.module @ctx(in %clk : !seq.clock, in %x : i4, out q : i4) {
  %i0 = seq.initial() {
    %c0 = hw.constant 0 : i4
    seq.yield %c0 : i4
  } : () -> !seq.immutable<i4>
  %c4 = hw.constant 4 : i4
  %e = comb.icmp eq %x, %c4 : i4
  %r = seq.compreg %nr, %clk initial %i0 : i4
  %nr = comb.mux %e, %x, %r : i4
  hw.output %r : i4
}
