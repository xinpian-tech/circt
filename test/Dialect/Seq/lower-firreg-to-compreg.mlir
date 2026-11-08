// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(seq-lower-firreg-to-compreg))' | FileCheck %s
// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(seq-lower-firreg-to-compreg{initial-reset=false free-initial=false}))' | FileCheck %s --check-prefix=ZERO

// CHECK-LABEL: hw.module @Regs
hw.module @Regs(in %clk : !seq.clock, in %rst : i1, in %d : i8, in %rv : i8,
                in %s : !hw.struct<a: i4, b: i4>,
                out o1 : i8, out o2 : i8, out o3 : i8, out o4 : i8,
                out o5 : i8, out o6 : !hw.struct<a: i4, b: i4>) {
  // Initial values are created at the start of the module.
  // CHECK: [[I7:%.+]] = seq.initial() {
  // CHECK-NEXT: hw.constant 7 : i8
  // CHECK: [[F0:%.+]] = seq.initial() {
  // CHECK-NEXT: hw.constant false
  // CHECK: [[FIRST:%.+]] = seq.compreg %true, %clk initial [[F0]] : i1
  // CHECK: [[I0:%.+]] = seq.initial() {
  // CHECK-NEXT: hw.constant 0 : i8
  // CHECK: [[I5:%.+]] = seq.initial() {
  // CHECK-NEXT: hw.constant 5 : i8
  %c5 = hw.constant 5 : i8

  // A constant sync reset: starts in the reset value.
  // CHECK: [[N1:%.+]] = comb.mux bin %rst, %c5_i8, %d
  // CHECK: %r1 = seq.compreg [[N1]], %clk initial [[I5]] : i8
  %r1 = seq.firreg %d clock %clk reset sync %rst, %c5 : i8

  // An async reset is also seen in the cycle where it is asserted.
  // CHECK: [[N2:%.+]] = comb.mux bin %rst, %c5_i8, %d
  // CHECK: %r2 = seq.compreg [[N2]], %clk initial [[I5]] : i8
  // CHECK: [[V2:%.+]] = comb.mux bin %rst, %c5_i8, %r2
  %r2 = seq.firreg %d clock %clk reset async %rst, %c5 : i8

  // A non-constant reset value is taken in the first cycle.
  // CHECK: [[N3:%.+]] = comb.mux bin %rst, %rv, %d
  // CHECK: %r3 = seq.compreg [[N3]], %clk initial [[I0]] : i8
  // CHECK: [[V3:%.+]] = comb.mux bin [[FIRST]], %r3, %rv
  %r3 = seq.firreg %d clock %clk reset sync %rst, %rv : i8

  // A preset is the initial value.
  // CHECK: %r4 = seq.compreg %d, %clk initial [[I7]] : i8
  %r4 = seq.firreg %d clock %clk preset 7 : i8

  // Without reset or preset, any value in the first cycle.
  // CHECK: [[ANY5:%.+]] = verif.symbolic_value : i8
  // CHECK: %r5 = seq.compreg %d, %clk initial [[I0]] : i8
  // CHECK: [[V5:%.+]] = comb.mux bin [[FIRST]], %r5, [[ANY5]]
  %r5 = seq.firreg %d clock %clk : i8

  // Aggregates are lowered on their bits.
  // CHECK: [[SI:%.+]] = hw.bitcast %s : (!hw.struct<a: i4, b: i4>) -> i8
  // CHECK: [[ANY6:%.+]] = verif.symbolic_value : i8
  // CHECK: %r6 = seq.compreg [[SI]], %clk initial [[I0]] : i8
  // CHECK: [[V6:%.+]] = comb.mux bin [[FIRST]], %r6, [[ANY6]]
  // CHECK: [[SO:%.+]] = hw.bitcast [[V6]] : (i8) -> !hw.struct<a: i4, b: i4>
  %r6 = seq.firreg %s clock %clk : !hw.struct<a: i4, b: i4>

  // CHECK: hw.output %r1, [[V2]], [[V3]], %r4, [[V5]], [[SO]]
  hw.output %r1, %r2, %r3, %r4, %r5, %r6 : i8, i8, i8, i8, i8, !hw.struct<a: i4, b: i4>
}

// Without initial-reset and free-initial, all registers without preset
// start at 0.
// ZERO-LABEL: hw.module @Regs
// ZERO-NOT: verif.symbolic_value
// ZERO-NOT: %first
// ZERO: %r1 = seq.compreg {{%.+}}, %clk initial [[I0:%.+]] : i8
// ZERO: %r4 = seq.compreg %d, %clk initial {{%.+}} : i8
