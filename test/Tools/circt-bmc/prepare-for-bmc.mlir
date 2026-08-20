// RUN: circt-opt %s --prepare-for-bmc="top-module=Top" | FileCheck %s

// CHECK-LABEL: hw.module @Top(
// CHECK-SAME:    in [[CLK:%[^:]+]] : !seq.clock
// CHECK-SAME:    in [[DATA:%[^:]+]] : i1
// CHECK:         [[RAW_CLK:%.+]] = seq.from_clock [[CLK]]
// CHECK-NOT:     seq.to_clock
// CHECK:         seq.firreg [[DATA]] clock [[CLK]]
hw.module @Top(in %clk: i1, in %data: i1) {
  %clock = seq.to_clock %clk
  %state = seq.firreg %data clock %clock : i1
}
