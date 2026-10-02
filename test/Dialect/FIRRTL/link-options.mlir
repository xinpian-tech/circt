// RUN: circt-opt %s --firrtl-link-circuits="base-circuit=Foo" | FileCheck %s

// CHECK-LABEL: firrtl.circuit "Foo"
// CHECK:         firrtl.option @Target {
// CHECK-NEXT:      firrtl.option_case @FPGA
// CHECK-NEXT:      firrtl.option_case @ASIC
// CHECK-NEXT:    }
// CHECK-NOT:     firrtl.option
firrtl.circuit "Foo" {
  firrtl.option @Target {
    firrtl.option_case @FPGA
  }
  firrtl.extmodule @FooFPGA()
  firrtl.extmodule @Bar()
  firrtl.module @Foo() {
    firrtl.instance_choice bar @Bar alternatives @Target { @FPGA -> @FooFPGA } ()
  }
}

firrtl.circuit "Bar" {
  firrtl.option @Target {
    firrtl.option_case @FPGA
    firrtl.option_case @ASIC
  }
  firrtl.extmodule @BarFPGA()
  firrtl.extmodule @BarASIC()
  firrtl.module @Bar() {
    firrtl.instance_choice baz @BarFPGA alternatives @Target { @FPGA -> @BarFPGA, @ASIC -> @BarASIC } ()
  }
}
