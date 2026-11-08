// RUN: circt-opt %s --synth-functional-reduction="test-transformation use-choices=false" | FileCheck %s

// Without choices, members are replaced by their representative (the first
// member of the class in topological order), or by its inverse.
// CHECK-LABEL: hw.module @replace
hw.module @replace(in %a: i1, in %b: i1, in %c: i1, out o0: i1, out o1: i1, out o2: i1, out o3: i1) {
  // CHECK:      %[[REP:.+]] = synth.aig.and_inv %a, %b {{.*}}: i1
  // CHECK-NEXT: %[[INV:.+]] = synth.aig.and_inv not %[[REP]] : i1
  // CHECK-NOT:  synth.choice
  // CHECK:      %[[USE:.+]] = synth.aig.and_inv %[[REP]], %c : i1
  // CHECK:      hw.output %[[REP]], %[[REP]], %[[INV]], %[[USE]]
  %0 = synth.aig.and_inv %a, %b {synth.test.fc_equiv_class = 0} : i1
  %1 = synth.aig.and_inv %b, %a {synth.test.fc_equiv_class = 0} : i1
  %3 = synth.aig.and_inv not %a : i1
  %4 = synth.aig.and_inv not %b : i1
  %2 = comb.or %3, %4 {synth.test.fc_equiv_class = 0} : i1
  %5 = synth.aig.and_inv %1, %c : i1
  hw.output %0, %1, %2, %5 : i1, i1, i1, i1
}
