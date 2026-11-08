// RUN: circt-opt --llhd-mem2reg %s | FileCheck %s

// Two processes drive one bit each of the same signal (one `always_ff` per
// bit), each through two equal projections in different branches. The
// projections are merged outside the processes so that each process drives
// its own bit, instead of reading back and driving the whole signal.
// CHECK-LABEL: @PerBit
hw.module @PerBit(in %clk: i1, in %d: i2, in %en: i1) {
  %c0 = hw.constant 0 : i1
  %c1 = hw.constant 1 : i1
  %c0_i2 = hw.constant 0 : i2
  %false = hw.constant false
  %t = llhd.constant_time <0ns, 1d>
  // CHECK: [[R:%.+]] = llhd.sig
  %r = llhd.sig %c0_i2 : <i2>
  // CHECK-NOT: llhd.drv [[R]],
  // CHECK: [[B0:%.+]] = llhd.sig.extract [[R]] from
  // CHECK-NEXT: llhd.process
  // CHECK-NOT: llhd.sig.extract
  // CHECK: llhd.drv [[B0]],
  // CHECK-NOT: llhd.drv [[R]],
  // CHECK: [[B1:%.+]] = llhd.sig.extract [[R]] from
  // CHECK-NEXT: llhd.process
  // CHECK-NOT: llhd.sig.extract
  // CHECK: llhd.drv [[B1]],
  llhd.process {
    cf.br ^bb1
  ^bb1:
    llhd.wait (%clk : i1), ^bb2
  ^bb2:
    cf.cond_br %en, ^bb3, ^bb4
  ^bb3:
    %p = llhd.sig.extract %r from %c0 : <i2> -> <i1>
    %v = comb.extract %d from 0 : (i2) -> i1
    llhd.drv %p, %v after %t : i1
    cf.br ^bb1
  ^bb4:
    %q = llhd.sig.extract %r from %c0 : <i2> -> <i1>
    llhd.drv %q, %false after %t : i1
    cf.br ^bb1
  }
  llhd.process {
    cf.br ^bb1
  ^bb1:
    llhd.wait (%clk : i1), ^bb2
  ^bb2:
    cf.cond_br %en, ^bb3, ^bb4
  ^bb3:
    %p = llhd.sig.extract %r from %c1 : <i2> -> <i1>
    %v = comb.extract %d from 1 : (i2) -> i1
    llhd.drv %p, %v after %t : i1
    cf.br ^bb1
  ^bb4:
    %q = llhd.sig.extract %r from %c1 : <i2> -> <i1>
    llhd.drv %q, %false after %t : i1
    cf.br ^bb1
  }
}

// A static slice is accessed through its elements, so that the process can be
// promoted (the promotion tree has no slice projection).
// CHECK-LABEL: @Slice
hw.module @Slice(in %a: !hw.array<3xi4>, in %u: !hw.array<2xi4>) {
  %c1 = hw.constant 1 : i2
  %s = llhd.sig %a : <!hw.array<3xi4>>
  // CHECK: llhd.combinational
  // CHECK-NOT: llhd.sig.array_slice
  // CHECK-NOT: llhd.prb
  // CHECK: llhd.yield
  %0 = llhd.combinational -> !hw.array<3xi4> {
    llhd.assign %s, %a : !hw.array<3xi4>
    %1 = llhd.sig.array_slice %s at %c1 : <!hw.array<3xi4>> -> <!hw.array<2xi4>>
    llhd.assign %1, %u : !hw.array<2xi4>
    %2 = llhd.prb %s : !hw.array<3xi4>
    llhd.yield %2 : !hw.array<3xi4>
  }
}
