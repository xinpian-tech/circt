// RUN: circt-opt %s --comb-context-simplify --canonicalize | FileCheck %s

// A priority decoder: x == 5 && !(x == 6) is x == 5.
// CHECK-LABEL: hw.module @decoder
// CHECK-NEXT:    [[C:%.+]] = hw.constant -3 : i3
// CHECK-NEXT:    [[E:%.+]] = comb.icmp eq %x, [[C]] : i3
// CHECK-NEXT:    hw.output [[E]]
hw.module @decoder(in %x : i3, out o : i1) {
  %c5 = hw.constant 5 : i3
  %c6 = hw.constant 6 : i3
  %true = hw.constant true
  %a = comb.icmp eq %x, %c5 : i3
  %b = comb.icmp eq %x, %c6 : i3
  %nb = comb.xor %b, %true : i1
  %o = comb.and %a, %nb : i1
  hw.output %o : i1
}

// A reset mux around logic that already selects the reset value under reset
// is that logic.
// CHECK-LABEL: hw.module @reset
// CHECK:         [[I:%.+]] = comb.mux %{{.+}}, %d, %c0_i8
// CHECK:         [[J:%.+]] = comb.mux %{{.+}}, %e, [[I]]
// CHECK-NEXT:    hw.output [[J]]
hw.module @reset(in %rst : i1, in %en : i1, in %d : i8, in %e : i8, out o : i8) {
  %c0 = hw.constant 0 : i8
  %c = comb.and %rst, %en : i1
  %i = comb.mux %c, %d, %c0 : i8
  %f = comb.and %rst, %en, %en : i1
  %j = comb.mux %f, %e, %i : i8
  %o = comb.mux %rst, %j, %c0 : i8
  hw.output %o : i8
}

// A case chain that already selects the value under the outer condition.
// CHECK-LABEL: hw.module @chain
// CHECK-NOT:     comb.mux %a,
// CHECK:         hw.output
hw.module @chain(in %x : i2, in %p : i8, in %q : i8, in %r : i8, out o : i8) {
  %c1 = hw.constant 1 : i2
  %c2 = hw.constant 2 : i2
  %a = comb.icmp eq %x, %c1 : i2
  %b = comb.icmp eq %x, %c2 : i2
  %inner = comb.mux %b, %q, %r : i8
  %chain = comb.mux %a, %p, %inner : i8
  %o = comb.mux %a, %p, %chain : i8
  hw.output %o : i8
}
