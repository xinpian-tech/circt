// RUN: circt-opt --lower-firrtl-to-hw --verify-diagnostics %s | FileCheck %s

// A verif.contract emitted directly into a FIRRTL module body must survive
// FIRRTL-to-HW lowering: operands/results are lowered and the body re-lowered.

firrtl.circuit "VerifContractDirect" {
  // CHECK-LABEL: hw.module @VerifContractDirect
  firrtl.module @VerifContractDirect(
      in %a: !firrtl.uint<42>, in %p: !firrtl.uint<1>, out %b: !firrtl.uint<42>) {
    // CHECK: [[R:%.+]] = verif.contract %a : i42 {
    %0 = verif.contract %a : !firrtl.uint<42> {
      // Frontend-inserted property type conversion: firrtl.uint<1> -> i1.
      %pi1 = builtin.unrealized_conversion_cast %p : !firrtl.uint<1> to i1
      // CHECK: verif.require %{{.+}} : i1
      verif.require %pi1 : i1
      // CHECK: verif.ensure %{{.+}} : i1
      verif.ensure %pi1 : i1
    // CHECK: }
    }
    // CHECK: hw.output [[R]] : i42
    firrtl.matchingconnect %b, %0 : !firrtl.uint<42>
  }

  // With optional block arguments, the body references the input through the
  // block argument (which dominates the body), not the contract's result.
  // CHECK-LABEL: hw.module @VerifContractBlockArgs
  firrtl.module @VerifContractBlockArgs(
      in %a: !firrtl.uint<42>, out %b: !firrtl.uint<42>) {
    // CHECK: [[R:%.+]] = verif.contract %a : i42 {
    %0 = verif.contract %a : !firrtl.uint<42> {
    ^bb0(%arg0: !firrtl.uint<42>):
      // CHECK: %{{.+}} = comb.extract
      %n = firrtl.node %arg0 : !firrtl.uint<42>
      %bit = firrtl.bits %n 0 to 0 : (!firrtl.uint<42>) -> !firrtl.uint<1>
      %biti1 = builtin.unrealized_conversion_cast %bit : !firrtl.uint<1> to i1
      // CHECK: verif.ensure %{{.+}} : i1
      verif.ensure %biti1 : i1
    // CHECK: }
    }
    // CHECK: hw.output [[R]] : i42
    firrtl.matchingconnect %b, %0 : !firrtl.uint<42>
  }
}
