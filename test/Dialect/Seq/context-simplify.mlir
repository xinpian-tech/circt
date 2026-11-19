// RUN: circt-opt %s --comb-context-simplify --canonicalize | FileCheck %s
// RUN: circt-opt %s --comb-context-simplify=or-form --canonicalize | FileCheck %s --check-prefix=OR

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
// is that logic; there the inner mux on the same condition is 0.
// CHECK-LABEL: hw.module @reset
// CHECK:         [[C:%.+]] = comb.and %rst, %en
// CHECK-NEXT:    [[J:%.+]] = comb.mux [[C]], %e, %c0_i8
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

// A disequality fact passes through a bijection with a constant: with
// rd != 0, 31 - rd is not 31 (the write enable of register 0 of a register
// file indexed from the top).
// CHECK-LABEL: hw.module @noteq
// CHECK-NEXT:    hw.output %r
hw.module @noteq(in %rd : i5, in %w : i8, in %r : i8, out o : i8) {
  %c0 = hw.constant 0 : i5
  %c31 = hw.constant 31 : i5
  %ne = comb.icmp ne %rd, %c0 : i5
  %idx = comb.sub %c31, %rd : i5
  %hit = comb.icmp eq %idx, %c31 : i5
  %slot = comb.mux %hit, %w, %r : i8
  %o = comb.mux %ne, %slot, %r : i8
  hw.output %o : i8
}

// The slices where the arms are equal (the slot that is not written): there
// the mux is the other arm.
// CHECK-LABEL: hw.module @slices
// CHECK-NOT:     comb.mux {{.+}} : i16
// CHECK:         [[LO:%.+]] = comb.extract %r from 0
// CHECK-NEXT:    [[O:%.+]] = comb.concat %{{.+}}, [[LO]] : i8, i8
// CHECK-NEXT:    hw.output [[O]]
hw.module @slices(in %rd : i1, in %en : i1, in %w : i8, in %r : i16, out o : i16) {
  %false = hw.constant false
  %hit = comb.icmp eq %rd, %false : i1
  %lo = comb.extract %r from 0 : (i16) -> i8
  %hi = comb.extract %r from 8 : (i16) -> i8
  %slot0 = comb.mux %hit, %w, %lo : i8
  %slot1 = comb.mux %rd, %w, %hi : i8
  %new = comb.concat %slot1, %slot0 : i8, i8
  %c = comb.and %en, %rd : i1
  %o = comb.mux %c, %new, %r : i16
  hw.output %o : i16
}

// A disjunction in the facts is split into its cases: with s == 1 or s == 2,
// none of the conditions of the chain holds (deeper than the splits on its
// conditions reach), so the chain is %p and the outer mux the chain.
// CHECK-LABEL: hw.module @clause
// CHECK-NOT:     comb.icmp eq %s, %c1_i4
// CHECK-NOT:     comb.icmp eq %s, %c-2_i4
// CHECK:         [[O:%.+]] = comb.mux %{{.+}}, %q, %p : i8
// CHECK-NEXT:    hw.output [[O]]
hw.module @clause(in %s : i4, in %p : i8, in %q : i8, out o : i8) {
  %c1 = hw.constant 1 : i4
  %c2 = hw.constant 2 : i4
  %c3 = hw.constant 3 : i4
  %c5 = hw.constant 5 : i4
  %c7 = hw.constant 7 : i4
  %c9 = hw.constant 9 : i4
  %c11 = hw.constant 11 : i4
  %a = comb.icmp eq %s, %c1 : i4
  %b = comb.icmp eq %s, %c2 : i4
  %ab = comb.or %a, %b : i1
  %e3 = comb.icmp eq %s, %c3 : i4
  %e5 = comb.icmp eq %s, %c5 : i4
  %e7 = comb.icmp eq %s, %c7 : i4
  %e9 = comb.icmp eq %s, %c9 : i4
  %e11 = comb.icmp eq %s, %c11 : i4
  %m11 = comb.mux %e11, %q, %p : i8
  %m9 = comb.mux %e9, %q, %m11 : i8
  %m7 = comb.mux %e7, %q, %m9 : i8
  %m5 = comb.mux %e5, %q, %m7 : i8
  %m3 = comb.mux %e3, %q, %m5 : i8
  %o = comb.mux %ab, %p, %m3 : i8
  hw.output %o : i8
}

// An operand of an or in an arm that is 0 where the arm is selected is
// dropped.
// CHECK-LABEL: hw.module @prune
// CHECK-NEXT:    [[O:%.+]] = comb.mux %c, %x, %z : i8
// CHECK-NEXT:    hw.output [[O]]
hw.module @prune(in %c : i1, in %e : i1, in %x : i8, in %w : i8, in %z : i8, out o : i8) {
  %true = hw.constant true
  %c0 = hw.constant 0 : i8
  %nc = comb.xor %c, %true : i1
  %d = comb.and %nc, %e : i1
  %y = comb.mux %d, %w, %c0 : i8
  %xy = comb.or %x, %y : i8
  %o = comb.mux %c, %xy, %z : i8
  hw.output %o : i8
}

// A mux in an arm whose condition is known where the arm is selected is its
// arm there (it stays for its other use).
// CHECK-LABEL: hw.module @resolve
// CHECK:         [[I:%.+]] = comb.mux %{{.+}}, %x, %y : i8
// CHECK-NEXT:    [[O:%.+]] = comb.mux %c, %x, %z : i8
// CHECK-NEXT:    hw.output [[O]], [[I]]
hw.module @resolve(in %c : i1, in %e : i1, in %x : i8, in %y : i8, in %z : i8, out o : i8, out p : i8) {
  %d = comb.or %c, %e : i1
  %i = comb.mux %d, %x, %y : i8
  %o = comb.mux %c, %i, %z : i8
  hw.output %o, %i : i8, i8
}

// A mux whose false arm is 0 where the true arm is selected is the or of its
// true arm gated by the condition and its false arm (an and less per bit).
// CHECK-LABEL: hw.module @disjoint
// CHECK:         [[O:%.+]] = comb.mux %{{.+}}, %x, %{{.+}} : i8
// CHECK-NEXT:    hw.output [[O]]
// OR-LABEL:      hw.module @disjoint
// OR-DAG:        [[G:%.+]] = comb.mux %{{.+}}, %y, %c0_i8 : i8
// OR-DAG:        [[X:%.+]] = comb.mux %{{.+}}, %x, %c0_i8 : i8
// OR:            [[O:%.+]] = comb.or [[X]], [[G]] : i8
// OR-NEXT:       hw.output [[O]]
hw.module @disjoint(in %s : i2, in %x : i8, in %y : i8, out o : i8) {
  %c1 = hw.constant 1 : i2
  %c2 = hw.constant 2 : i2
  %c0 = hw.constant 0 : i8
  %a = comb.icmp eq %s, %c1 : i2
  %b = comb.icmp eq %s, %c2 : i2
  %g = comb.mux %b, %y, %c0 : i8
  %o = comb.mux %a, %x, %g : i8
  hw.output %o : i8
}

// Bits of the arms that are constant are left to the mux.
// OR-LABEL:      hw.module @disjointConstant
// OR-NOT:        comb.or
// OR:            hw.output
hw.module @disjointConstant(in %c : i1, in %x : i4, in %y : i8, out o : i8) {
  %c0 = hw.constant 0 : i4
  %a = comb.concat %c0, %x : i4, i4
  %o = comb.mux %c, %a, %y : i8
  hw.output %o : i8
}
