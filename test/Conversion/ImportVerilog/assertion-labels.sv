// RUN: circt-verilog --ir-moore %s | FileCheck %s --check-prefix=MOORE
// RUN: circt-verilog %s | FileCheck %s
// REQUIRES: slang

// Statement labels of assertions become the label of the assertion op.
// CHECK-LABEL: hw.module @Labels
module Labels(input logic clk, input logic a, input logic b);
  // CHECK: verif.clocked_assert {{%.+}}, posedge %clk label "a_foo"
  a_foo: assert property (@(posedge clk) a |-> b);
  // CHECK: verif.clocked_assume %a, posedge %clk label "m_baz"
  m_baz: assume property (@(posedge clk) a);
  // CHECK: verif.clocked_cover %b, posedge %clk label "c_qux"
  c_qux: cover property (@(posedge clk) b);
  // CHECK: verif.clocked_assert %b, posedge %clk :
  assert property (@(posedge clk) b);
  always_comb begin
    // MOORE: moore.assert immediate {{%.+}} label "a_bar"
    a_bar: assert (a);
  end
endmodule
