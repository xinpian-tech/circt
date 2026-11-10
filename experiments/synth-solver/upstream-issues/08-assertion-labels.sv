// Statement labels of assertions.
module top(input logic clk, input logic a, input logic b);
  a_foo: assert property (@(posedge clk) a |-> b);
  m_baz: assume property (@(posedge clk) a);
  always_comb begin
    a_bar: assert (a);
  end
endmodule
