// RUN: split-file %s %t
// RUN: firld %t/Top.mlir %t/Cell.mlir --base-circuit Top | FileCheck %s
// RUN: firld %t/Cell.mlir %t/Top.mlir --base-circuit Top | FileCheck %s
// RUN: not firld %t/Top.mlir %t/Cell.mlir %t/Other.mlir --base-circuit Top 2>&1 | FileCheck %s --check-prefix=ERROR

// CHECK:     firrtl.extmodule @Cell(in a: !firrtl.uint<1>) attributes {convention = #firrtl<convention scalarized>, defname = "CKBUFX1"}
// CHECK-NOT: firrtl.extmodule {{.*}}@Cell(

// ERROR: has colliding symbol Cell

//--- Top.mlir
module {
  firrtl.circuit "Top" {
    firrtl.module @Top(in %a: !firrtl.uint<1>) {
      %cell_a = firrtl.instance cell @Cell(in a: !firrtl.uint<1>)
      firrtl.matchingconnect %cell_a, %a : !firrtl.uint<1>
    }
    firrtl.extmodule private @Cell(in a: !firrtl.uint<1>) attributes {defname = "Cell"}
  }
}

//--- Cell.mlir
module {
  firrtl.circuit "Cell" {
    firrtl.extmodule @Cell(in a: !firrtl.uint<1>) attributes {convention = #firrtl<convention scalarized>, defname = "CKBUFX1"}
  }
}

//--- Other.mlir
module {
  firrtl.circuit "Other" {
    firrtl.extmodule @Other()
    firrtl.extmodule @Cell(in a: !firrtl.uint<1>) attributes {defname = "Cell2"}
  }
}
