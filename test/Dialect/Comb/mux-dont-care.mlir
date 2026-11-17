// RUN: circt-opt %s --comb-mux-dont-care | FileCheck %s
// RUN: circt-opt %s --comb-mux-dont-care=max-support-bits=2 | FileCheck %s --check-prefix=LIMIT

// The realignment of a misaligned load: the held word is only selected when
// the offset is not zero, so its low byte is never shifted into the result.
// The other arm can be shifted by any offset, so only its top byte is unseen.
// CHECK-LABEL: hw.module @load
// CHECK-DAG:     %[[FULL:.+]] = comb.concat %resp, %held : i32, i32
// CHECK-DAG:     %[[REP:.+]] = comb.replicate %resp : (i32) -> i64
// CHECK-DAG:     %[[LOW:.+]] = comb.extract %[[REP]] from 0 : (i64) -> i56
// CHECK-DAG:     %[[FALSE:.+]] = comb.concat %{{.+}}, %[[LOW]] : i8, i56
// CHECK-DAG:     %[[MID:.+]] = comb.extract %[[FULL]] from 8 : (i64) -> i48
// CHECK-DAG:     %[[TRUE:.+]] = comb.concat %{{.+}}, %[[MID]], %{{.+}} : i8, i48, i8
// CHECK:         comb.mux %{{.+}}, %[[TRUE]], %[[FALSE]] : i64
// LIMIT-LABEL: hw.module @load
// LIMIT:         %[[FULL:.+]] = comb.concat %resp, %held : i32, i32
// LIMIT:         %[[REP:.+]] = comb.replicate %resp : (i32) -> i64
// LIMIT:         comb.mux %{{.+}}, %[[FULL]], %[[REP]] : i64
hw.module @load(in %resp : i32, in %held : i32, in %size : i2, in %offset : i2, out out : i32) {
  %c0_i2 = hw.constant 0 : i2
  %c-2_i2 = hw.constant -2 : i2
  %c1_i2 = hw.constant 1 : i2
  %c-1_i2 = hw.constant -1 : i2
  %c0_i3 = hw.constant 0 : i3
  %c0_i59 = hw.constant 0 : i59
  %word = comb.icmp eq %size, %c-2_i2 : i2
  %nonzero = comb.icmp ne %offset, %c0_i2 : i2
  %a = comb.and %word, %nonzero : i1
  %half = comb.icmp eq %size, %c1_i2 : i2
  %three = comb.icmp eq %offset, %c-1_i2 : i2
  %b = comb.and %half, %three : i1
  %split = comb.or %a, %b : i1
  %full = comb.concat %resp, %held : i32, i32
  %rep = comb.replicate %resp : (i32) -> i64
  %mux = comb.mux %split, %full, %rep : i64
  %k = comb.concat %c0_i59, %offset, %c0_i3 : i59, i2, i3
  %shifted = comb.shru %mux, %k : i64
  %out = comb.extract %shifted from 0 : (i64) -> i32
  hw.output %out : i32
}

// A mux with another user keeps its arms.
// CHECK-LABEL: hw.module @otherUser
// CHECK:         comb.mux %c, %a, %b : i8
// CHECK-NOT:     comb.concat %{{.+}} : i4, i4
hw.module @otherUser(in %c : i1, in %a : i8, in %b : i8, in %k : i3, out o0 : i4, out o1 : i8) {
  %c0_i5 = hw.constant 0 : i5
  %mux = comb.mux %c, %a, %b : i8
  %k8 = comb.concat %c0_i5, %k : i5, i3
  %s = comb.shru %mux, %k8 : i8
  %o0 = comb.extract %s from 0 : (i8) -> i4
  hw.output %o0, %mux : i4, i8
}

// A left shift by 4 when the select is set: the high half of the true arm is
// shifted out. The false arm is not shifted and keeps all its bits.
// CHECK-LABEL: hw.module @left
// CHECK-DAG:     %[[C0_4:.+]] = hw.constant 0 : i4
// CHECK-DAG:     %[[A:.+]] = comb.extract %a from 0 : (i8) -> i4
// CHECK-DAG:     %[[T:.+]] = comb.concat %[[C0_4]], %[[A]] : i4, i4
// CHECK:         comb.mux %c, %[[T]], %b : i8
hw.module @left(in %c : i1, in %a : i8, in %b : i8, out o : i8) {
  %c4_i8 = hw.constant 4 : i8
  %c0_i8 = hw.constant 0 : i8
  %k = comb.mux %c, %c4_i8, %c0_i8 : i8
  %mux = comb.mux %c, %a, %b : i8
  %s = comb.shl %mux, %k : i8
  hw.output %s : i8
}
