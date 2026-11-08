// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-remove-false-cycles))' --verify-diagnostics | FileCheck %s
// REQUIRES: cadical

// A cycle of ops but not of bits: bit 1 of %x is computed from bit 0, which
// comes from an input. Rebuilding bit by bit removes it without a SAT check.
// CHECK-LABEL: hw.module @WordCycle
hw.module @WordCycle(in %a : i1, in %b : i1, out o : i2) {
  // CHECK-DAG: [[HI:%.+]] = synth.aig.and_inv %a, %b : i1
  // CHECK-DAG: [[X:%.+]] = comb.concat [[HI]], %a : i1, i1
  // CHECK: hw.output [[X]]
  %lo = comb.extract %x from 0 : (i2) -> i1
  %hi = synth.aig.and_inv %lo, %b : i1
  %x = comb.concat %hi, %a : i1, i1
  hw.output %x : i2
}

// valid = in & !kill, kill = step & valid & !in: a cycle of bits, but kill
// is 0 whenever in is 1, so valid = in. The cut bit is computed by one copy
// of the cycle with the cut bit at 0.
// CHECK-LABEL: hw.module @FalseCycle
hw.module @FalseCycle(in %in : i1, in %step : i1, out valid : i1) {
  // CHECK-NOT: comb.extract
  // CHECK-DAG: [[F:%.+]] = hw.constant false
  // CHECK-DAG: [[K1:%.+]] = synth.aig.and_inv %step, [[F]], not %in : i1
  // CHECK-DAG: [[V1:%.+]] = synth.aig.and_inv %in, not [[K1]] : i1
  // CHECK-DAG: [[K:%.+]] = synth.aig.and_inv %step, [[V1]], not %in : i1
  // CHECK-DAG: [[V:%.+]] = synth.aig.and_inv %in, not [[K]] : i1
  // CHECK: hw.output [[V]]
  %valid = synth.aig.and_inv %in, not %kill : i1
  %kill = synth.aig.and_inv %step, %valid, not %in : i1
  hw.output %valid : i1
}

// The same through a word: w[1] = in & !w[0], w[0] = step & w[1] & !in.
// CHECK-LABEL: hw.module @FalseCycleWord
hw.module @FalseCycleWord(in %in : i1, in %step : i1, out o : i2) {
  // CHECK-NOT: comb.extract %w
  // CHECK: hw.output
  %v = comb.extract %w from 1 : (i2) -> i1
  %k = comb.extract %w from 0 : (i2) -> i1
  %nk = synth.aig.and_inv not %k : i1
  %nin = synth.aig.and_inv not %in : i1
  %true = hw.constant true
  %a = comb.concat %in, %step : i1, i1
  %b = comb.concat %nk, %v : i1, i1
  %c = comb.concat %true, %nin : i1, i1
  %w = synth.aig.and_inv %a, %b, %c : i2
  hw.output %w : i2
}

// The same with the cycle closed through two fixed points (in = step = 1):
// w[1] = in & !w[0], w[0] = step & !w[1] & in.
// CHECK-LABEL: hw.module @TwoFixedPoints
// expected-warning @below {{combinational cycle of 2 bits depends on itself; left in place}}
hw.module @TwoFixedPoints(in %in : i1, in %step : i1, out o : i2) {
  %v = comb.extract %w from 1 : (i2) -> i1
  %k = comb.extract %w from 0 : (i2) -> i1
  %a = comb.concat %in, %step : i1, i1
  %b = comb.concat %k, %v : i1, i1
  %true = hw.constant true
  %c = comb.concat %true, %in : i1, i1
  // CHECK: synth.aig.and_inv %{{.+}}, not %{{.+}}, %{{.+}} : i2
  // expected-note @below {{bit 1 is part of the cycle}}
  // expected-note @below {{bit 0 is part of the cycle}}
  %w = synth.aig.and_inv %a, not %b, %c : i2
  hw.output %w : i2
}

// A latch: x = a | (b & x) has two fixed points when a = 0 and b = 1.
// CHECK-LABEL: hw.module @Latch
// expected-warning @below {{combinational cycle of 3 bits depends on itself; left in place}}
hw.module @Latch(in %a : i1, in %b : i1, out o : i1) {
  // CHECK: synth.aig.and_inv %b, %{{.+}} : i1
  // expected-note @below {{is part of the cycle}}
  %t = synth.aig.and_inv %b, %x : i1
  // expected-note @below {{is part of the cycle}}
  %nx = synth.aig.and_inv not %a, not %t : i1
  // expected-note @below {{is part of the cycle}}
  %x = synth.aig.and_inv not %nx : i1
  hw.output %x : i1
}

// A ring oscillator has no fixed point.
// CHECK-LABEL: hw.module @Oscillator
// expected-warning @below {{combinational cycle of 1 bits depends on itself; left in place}}
hw.module @Oscillator(out o : i1) {
  // expected-note @below {{is part of the cycle}}
  %x = synth.aig.and_inv not %x : i1
  hw.output %x : i1
}
