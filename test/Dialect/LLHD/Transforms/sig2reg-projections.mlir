// RUN: circt-opt --llhd-sig2reg %s | FileCheck %s

// A signal with a single drive whose parts are only read through projections
// is replaced by its value, and the probes by the parts of the value.
// CHECK-LABEL: @ProbedParts
hw.module @ProbedParts(in %a: !hw.array<4xi8>, in %b: !hw.struct<x: i4, y: i4>,
                       in %c: i8, out o0: i8, out o1: !hw.array<2xi8>,
                       out o2: i4, out o3: i2) {
  %c1_i2 = hw.constant 1 : i2
  %c2_i3 = hw.constant 2 : i3
  // CHECK-NOT: llhd.sig
  %s = llhd.sig %a : <!hw.array<4xi8>>
  llhd.assign %s, %a : !hw.array<4xi8>
  // CHECK-DAG: [[GET:%.+]] = hw.array_get %a[%c1_i2]
  %0 = llhd.sig.array_get %s[%c1_i2] : <!hw.array<4xi8>>
  %1 = llhd.prb %0 : i8
  // CHECK-DAG: [[SLICE:%.+]] = hw.array_slice %a[%c1_i2]
  %2 = llhd.sig.array_slice %s at %c1_i2 : <!hw.array<4xi8>> -> <!hw.array<2xi8>>
  %3 = llhd.prb %2 : !hw.array<2xi8>
  %u = llhd.sig %b : <!hw.struct<x: i4, y: i4>>
  llhd.assign %u, %b : !hw.struct<x: i4, y: i4>
  // CHECK-DAG: [[FIELD:%.+]] = hw.struct_extract %b["y"]
  %4 = llhd.sig.struct_extract %u["y"] : <!hw.struct<x: i4, y: i4>>
  %5 = llhd.prb %4 : i4
  %v = llhd.sig %c : <i8>
  llhd.assign %v, %c : i8
  // CHECK-DAG: [[BITS:%.+]] = comb.extract %c from 2 : (i8) -> i2
  %6 = llhd.sig.extract %v from %c2_i3 : <i8> -> <i2>
  %7 = llhd.prb %6 : i2
  // CHECK: hw.output [[GET]], [[SLICE]], [[FIELD]], [[BITS]]
  hw.output %1, %3, %5, %7 : i8, !hw.array<2xi8>, i4, i2
}
