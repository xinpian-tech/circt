// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-remove-false-cycles{obligation}))' 2>/dev/null | FileCheck %s --check-prefix=UNROLL
// RUN: circt-opt %s --pass-pipeline='builtin.module(hw.module(synth-remove-false-cycles{obligation fixpoint-obligation}))' 2>/dev/null | FileCheck %s --check-prefix=FIX
// REQUIRES: cadical

// x = !(s & !x) = !s | x: one fixed point (x = 1) when s = 0, two when
// s = 1. The check fails; with an obligation the cycle is rebuilt anyway and
// the obligation output is set where the rebuilt value may be wrong.
// UNROLL-LABEL: hw.module @Cycle
// UNROLL-SAME: in %false_cycle_y1_0 : i1, in %false_cycle_y2_0 : i1
// UNROLL-SAME: out false_cycle_obligation : i1
// The cycle logic applied to 0, Y1 and Y2 (the cut bit is %a).
// UNROLL-DAG: synth.aig.and_inv not %false : i1
// UNROLL-DAG: synth.aig.and_inv not %false_cycle_y1_0 : i1
// UNROLL-DAG: synth.aig.and_inv not %false_cycle_y2_0 : i1
// UNROLL: hw.output {{%.+}}, {{%.+}} : i1, i1
// FIX-LABEL: hw.module @Cycle
// FIX-SAME: (in %s : i1, out o : i1, out false_cycle_obligation : i1)
// The cycle logic applied to both values of the cut bit.
// FIX-DAG: synth.aig.and_inv not %false : i1
// FIX-DAG: synth.aig.and_inv not %true : i1
// FIX: hw.output {{%.+}}, {{%.+}} : i1, i1
hw.module @Cycle(in %s : i1, out o : i1) {
  %a = synth.aig.and_inv %s, not %x : i1
  %x = synth.aig.and_inv not %a : i1
  hw.output %x : i1
}
