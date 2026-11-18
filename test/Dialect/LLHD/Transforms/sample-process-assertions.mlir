// RUN: circt-opt --llhd-sample-process-assertions %s | FileCheck %s

// The assertion of a process on the rising edge of %clock moves to a copy of
// the process run as at a rising edge; the process keeps its drives.
// CHECK-LABEL: @Clocked
hw.module @Clocked(in %clock : i1, in %check : i1, in %a : i1, in %d : i4) {
  %true = hw.constant true
  %false = hw.constant false
  %c0_i4 = hw.constant 0 : i4
  // CHECK: llhd.combinational {
  // CHECK:   [[LOW:%.+]] = hw.constant false
  // CHECK:   [[HIGH:%.+]] = hw.constant true
  // CHECK:   cf.br [[RESUME:\^.+]]([[LOW]] : i1)
  // CHECK:   llhd.yield
  // CHECK: [[RESUME]]([[PAST:%.+]]: i1):
  // CHECK:   [[NP:%.+]] = comb.xor bin [[PAST]], %true
  // CHECK:   [[EDGE:%.+]] = comb.and bin [[NP]], [[HIGH]]
  // CHECK:   cf.cond_br [[EDGE]]
  // CHECK:   verif.assert %a label "x" : i1
  // CHECK: }
  // CHECK: llhd.process
  // CHECK-NOT: verif.assert
  // CHECK: llhd.wait
  %0:2 = llhd.process -> i4, i1 {
    cf.br ^bb1(%c0_i4, %false : i4, i1)
  ^bb1(%1: i4, %2: i1):
    llhd.wait yield (%1, %2 : i4, i1), (%clock : i1), ^bb2(%clock : i1)
  ^bb2(%3: i1):
    %4 = comb.xor bin %3, %true : i1
    %5 = comb.and bin %4, %clock : i1
    cf.cond_br %5, ^bb3, ^bb1(%c0_i4, %false : i4, i1)
  ^bb3:
    cf.cond_br %check, ^bb4, ^bb1(%d, %true : i4, i1)
  ^bb4:
    verif.assert %a label "x" : i1
    cf.br ^bb1(%c0_i4, %false : i4, i1)
  }
  %s = llhd.sig %c0_i4 : <i4>
  %t = llhd.constant_time <0ns, 1d>
  llhd.drv %s, %0#0 after %t if %0#1 : i4
}

// A process waiting on a falling edge is left alone.
// CHECK-LABEL: @Falling
hw.module @Falling(in %clock : i1, in %a : i1) {
  %true = hw.constant true
  // CHECK-NOT: llhd.combinational
  // CHECK: verif.assert %a
  llhd.process {
    cf.br ^bb1
  ^bb1:
    llhd.wait (%clock : i1), ^bb2(%clock : i1)
  ^bb2(%0: i1):
    %1 = comb.xor bin %clock, %true : i1
    %2 = comb.and bin %0, %1 : i1
    cf.cond_br %2, ^bb3, ^bb1
  ^bb3:
    verif.assert %a : i1
    cf.br ^bb1
  }
}
