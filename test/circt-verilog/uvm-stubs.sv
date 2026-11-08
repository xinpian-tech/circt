// RUN: circt-verilog --uvm-stubs --ir-moore %s | FileCheck %s
// RUN: not circt-verilog --ir-moore %s 2>&1 | FileCheck %s --check-prefix=NOSTUBS
// REQUIRES: slang

// NOSTUBS: error: unknown package 'uvm_pkg'

// CHECK-LABEL: moore.module @UvmStubs
module UvmStubs(input logic clk, input logic a);
  import uvm_pkg::*;
  // CHECK: verif.clocked_assert
  a_high: assert property (@(posedge clk) a)
    else `uvm_error("UvmStubs", "a is low")
  always_comb begin
    // CHECK: moore.fmt.literal "low"
    // CHECK: moore.builtin.severity info
    if (!a) `uvm_info("UvmStubs", "low", UVM_LOW)
    // CHECK: moore.fmt.literal "warn"
    // CHECK: moore.builtin.severity warning
    if (!a) `uvm_warning("UvmStubs", "warn")
  end
endmodule
