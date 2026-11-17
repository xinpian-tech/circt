// RUN: circt-opt %s --seq-trim-registers | FileCheck %s

// The low byte of the held word only feeds itself: it is removed, and reads
// as zero. The high bytes reach the output.
// CHECK-LABEL: hw.module @hold
// CHECK:         %[[NEXT:.+]] = comb.extract %{{.+}} from 8 : (i32) -> i24
// CHECK:         %[[INIT:.+]] = seq.initial() {
// CHECK-NEXT:      hw.constant 5 : i24
// CHECK:         %held = seq.compreg %[[NEXT]], %clk initial %[[INIT]] : i24
// CHECK:         %[[C0:.+]] = hw.constant 0 : i8
// CHECK:         comb.concat %held, %[[C0]] : i24, i8
hw.module @hold(in %clk : !seq.clock, in %en : i1, in %d : i32, out o : i24) {
  %init = seq.initial() {
    %c = hw.constant 1280 : i32
    seq.yield %c : i32
  } : () -> !seq.immutable<i32>
  %held = seq.compreg name "held" %next, %clk initial %init : i32
  %next = comb.mux %en, %d, %held : i32
  %o = comb.extract %held from 8 : (i32) -> i24
  hw.output %o : i24
}

// A register read by an unknown operator keeps all bits, one read by an
// assertion through an extract keeps that bit, and one nothing reads is
// removed.
// CHECK-LABEL: hw.module @observers
// CHECK:         %a = seq.compreg %d, %clk : i8
// CHECK:         %[[B0:.+]] = comb.extract %d from 0 : (i8) -> i1
// CHECK:         %b = seq.compreg %[[B0]], %clk : i1
// CHECK-NOT:     seq.compreg
hw.module @observers(in %clk : !seq.clock, in %d : i8, out o : i1) {
  %a = seq.compreg %d, %clk : i8
  %b = seq.compreg %d, %clk : i8
  %c = seq.compreg %d, %clk : i8
  %c0 = hw.constant 0 : i8
  %eq = comb.icmp eq %a, %c0 : i8
  %bit = comb.extract %b from 0 : (i8) -> i1
  verif.assert %bit : i1
  hw.output %eq : i1
}

// An adder only carries upwards: the high bits of its operands are not
// observed through its low bits.
// CHECK-LABEL: hw.module @adder
// CHECK:         %[[X:.+]] = comb.extract %x from 0 : (i8) -> i4
// CHECK:         %r = seq.compreg %[[X]], %clk : i4
hw.module @adder(in %clk : !seq.clock, in %x : i8, out o : i4) {
  %r = seq.compreg %x, %clk : i8
  %one = hw.constant 1 : i8
  %s = comb.add %r, %one : i8
  %o = comb.extract %s from 0 : (i8) -> i4
  hw.output %o : i4
}
