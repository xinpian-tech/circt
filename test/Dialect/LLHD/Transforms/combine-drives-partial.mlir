// RUN: circt-opt --llhd-combine-drives %s | FileCheck %s --check-prefixes=CHECK,KEEP
// RUN: circt-opt --llhd-combine-drives=free-undriven %s | FileCheck %s --check-prefixes=CHECK,FREE

// A signal without initial value with an undriven bit: left alone, or with
// free-undriven, the bit takes any value.
// CHECK-LABEL: @Undriven
hw.module @Undriven(in %u0: i1, in %u1: i1, out o: i3) {
  %c0_i2 = hw.constant 0 : i2
  %c1_i2 = hw.constant 1 : i2
  // FREE: %s = llhd.sig : <i3>
  // FREE-NEXT: [[ALL:%.+]] = comb.concat [[HI:%.+]], %u1, %u0
  // FREE-NEXT: llhd.assign %s, [[ALL]] : i3
  // FREE-NEXT: [[ANY:%.+]] = verif.symbolic_value : i3
  // FREE-NEXT: [[HI]] = comb.extract [[ANY]] from 2 : (i3) -> i1
  // KEEP: llhd.assign {{%.+}}, %u0 : i1
  // KEEP: llhd.assign {{%.+}}, %u1 : i1
  %s = llhd.sig : <i3>
  %s0 = llhd.sig.extract %s from %c0_i2 : <i3> -> <i1>
  %s1 = llhd.sig.extract %s from %c1_i2 : <i3> -> <i1>
  llhd.assign %s0, %u0 : i1
  llhd.assign %s1, %u1 : i1
  %p = llhd.prb %s : i3
  hw.output %p : i3
}

// A drive of the whole struct that passes the field driven elsewhere through
// (a read-modify-write of field a, while b has its own assignment) fills in
// the rest.
// CHECK-LABEL: @ReadModifyWrite
hw.module @ReadModifyWrite(in %u: i4, in %v: i4, out o: !hw.struct<a: i4, b: i4>) {
  // CHECK: %s = llhd.sig : <!hw.struct<a: i4, b: i4>>
  // CHECK-NEXT: [[S:%.+]] = hw.struct_create ([[A:%.+]], %v)
  // CHECK-NEXT: llhd.assign %s, [[S]]
  // CHECK-NOT: llhd.assign
  // CHECK: [[W:%.+]] = hw.struct_inject {{%.+}}["a"], %u
  // CHECK-NEXT: [[A]] = hw.struct_extract [[W]]["a"]
  %s = llhd.sig : <!hw.struct<a: i4, b: i4>>
  %p = llhd.prb %s : !hw.struct<a: i4, b: i4>
  %w = hw.struct_inject %p["a"], %u : !hw.struct<a: i4, b: i4>
  llhd.assign %s, %w : !hw.struct<a: i4, b: i4>
  %b = llhd.sig.struct_extract %s["b"] : <!hw.struct<a: i4, b: i4>>
  llhd.assign %b, %v : i4
  hw.output %p : !hw.struct<a: i4, b: i4>
}

// The whole drive assigns field b too: two drivers, left alone.
// CHECK-LABEL: @Conflict
hw.module @Conflict(in %u: i4, in %v: i4, out o: !hw.struct<a: i4, b: i4>) {
  // CHECK: [[W:%.+]] = hw.struct_create (%u, %u)
  // CHECK-NEXT: llhd.assign %s, [[W]]
  // CHECK-NEXT: llhd.sig.struct_extract
  // CHECK-NEXT: llhd.assign {{%.+}}, %v : i4
  %s = llhd.sig : <!hw.struct<a: i4, b: i4>>
  %p = llhd.prb %s : !hw.struct<a: i4, b: i4>
  %w = hw.struct_create (%u, %u) : !hw.struct<a: i4, b: i4>
  llhd.assign %s, %w : !hw.struct<a: i4, b: i4>
  %b = llhd.sig.struct_extract %s["b"] : <!hw.struct<a: i4, b: i4>>
  llhd.assign %b, %v : i4
  hw.output %p : !hw.struct<a: i4, b: i4>
}
