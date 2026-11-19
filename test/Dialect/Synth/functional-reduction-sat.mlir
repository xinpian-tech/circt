// REQUIRES: cadical
// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-functional-reduction{use-choices=false num-random-patterns=64}))' | FileCheck %s
// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-functional-reduction{use-choices=false num-random-patterns=64 refine=false}))' | FileCheck %s --check-prefix=NOREFINE
// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-functional-reduction{use-choices=false num-random-patterns=64}))' --mlir-pass-statistics 2>&1 >/dev/null | FileCheck %s --check-prefix=STATS

// The values on the combinational cycle (%x, %y) are cut points: free inputs
// for simulation and SAT that are neither merged nor representatives. %p and
// %q are equivalent for any value of %x and are merged; %p is also
// equivalent to %x (x = a & y), which is not used.
// CHECK-LABEL: hw.module @cycle
// CHECK:      %[[X:.+]] = synth.aig.and_inv %a, %[[Y:.+]] : i1
// CHECK-NEXT: %[[Y]] = synth.aig.and_inv %b, %[[X]] : i1
// CHECK-NEXT: %[[P:.+]] = synth.aig.and_inv %[[X]], %a : i1
// CHECK-NEXT: hw.output %[[P]], %[[P]], %[[Y]]
// STATS: 2 num-cut-values
hw.module @cycle(in %a: i1, in %b: i1, out o0: i1, out o1: i1, out o2: i1) {
  %x = synth.aig.and_inv %a, %y : i1
  %y = synth.aig.and_inv %b, %x : i1
  %p = synth.aig.and_inv %x, %a : i1
  %q = synth.aig.and_inv %a, %x : i1
  hw.output %p, %q, %y : i1, i1, i1
}

// A gate that only depends on the cycle through a register is not cut.
// CHECK-LABEL: hw.module @register_loop
// CHECK:      %[[R:.+]] = seq.compreg %[[N:.+]], %clk : i1
// CHECK-NEXT: %[[N]] = synth.aig.and_inv %a, %[[R]] : i1
// CHECK-NEXT: hw.output %[[N]], %[[N]]
hw.module @register_loop(in %clk: !seq.clock, in %a: i1, out o0: i1, out o1: i1) {
  %r = seq.compreg %n, %clk : i1
  %n = synth.aig.and_inv %a, %r : i1
  %m = synth.aig.and_inv %r, %a : i1
  hw.output %n, %m : i1, i1
}

// All of %r16, %m1 and %m2 are 0 in the random patterns (they are 1 in one
// of 2^16 or 2^17 input patterns), so they are candidates for the constant
// false. The counterexample of %r16 separates them from it, and %m1 and %m2,
// which are equal, are only merged after refinement.
// CHECK-LABEL: hw.module @refine
// CHECK:      %[[M1:.+]] = synth.aig.and_inv %[[R16:.+]], %y : i1
// CHECK:      hw.output %[[R16]], %[[M1]], %[[M1]]
// NOREFINE-LABEL: hw.module @refine
// NOREFINE:      %[[M1:.+]] = synth.aig.and_inv %[[R16:.+]], %y : i1
// NOREFINE-NEXT: %[[D:.+]] = synth.aig.and_inv %{{.+}}, %y : i1
// NOREFINE-NEXT: %[[M2:.+]] = synth.aig.and_inv %{{.+}}, %[[D]] : i1
// NOREFINE-NEXT: hw.output %[[R16]], %[[M1]], %[[M2]]
hw.module @refine(in %x0: i1, in %x1: i1, in %x2: i1, in %x3: i1, in %x4: i1,
                  in %x5: i1, in %x6: i1, in %x7: i1, in %x8: i1, in %x9: i1,
                  in %x10: i1, in %x11: i1, in %x12: i1, in %x13: i1,
                  in %x14: i1, in %x15: i1, in %y: i1,
                  out o0: i1, out o1: i1, out o2: i1) {
  %a0 = synth.aig.and_inv %x0, %x1 : i1
  %a1 = synth.aig.and_inv %x2, %x3 : i1
  %a2 = synth.aig.and_inv %x4, %x5 : i1
  %a3 = synth.aig.and_inv %x6, %x7 : i1
  %a4 = synth.aig.and_inv %x8, %x9 : i1
  %a5 = synth.aig.and_inv %x10, %x11 : i1
  %a6 = synth.aig.and_inv %x12, %x13 : i1
  %a7 = synth.aig.and_inv %x14, %x15 : i1
  %b0 = synth.aig.and_inv %a0, %a1 : i1
  %b1 = synth.aig.and_inv %a2, %a3 : i1
  %b2 = synth.aig.and_inv %a4, %a5 : i1
  %b3 = synth.aig.and_inv %a6, %a7 : i1
  %c0 = synth.aig.and_inv %b0, %b1 : i1
  %c1 = synth.aig.and_inv %b2, %b3 : i1
  %r16 = synth.aig.and_inv %c0, %c1 : i1
  %m1 = synth.aig.and_inv %r16, %y : i1
  %d = synth.aig.and_inv %c1, %y : i1
  %m2 = synth.aig.and_inv %c0, %d : i1
  hw.output %r16, %m1, %m2 : i1, i1, i1
}

// Extracts of the same bit are the same input, even if not CSE'd.
// CHECK-LABEL: hw.module @duplicate_extract
// CHECK:      %[[E:.+]] = comb.extract %a from 0 : (i2) -> i1
// CHECK:      %[[N:.+]] = synth.aig.and_inv %[[E]], %b : i1
// CHECK-NEXT: hw.output %[[N]], %[[N]]
hw.module @duplicate_extract(in %a: i2, in %b: i1, out o0: i1, out o1: i1) {
  %e0 = comb.extract %a from 0 : (i2) -> i1
  %e1 = comb.extract %a from 0 : (i2) -> i1
  %n0 = synth.aig.and_inv %e0, %b : i1
  %n1 = synth.aig.and_inv %b, %e1 : i1
  hw.output %n0, %n1 : i1, i1
}
