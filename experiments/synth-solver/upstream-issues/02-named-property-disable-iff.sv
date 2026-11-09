// Clocking and disable iff given inside a named property.
module top(input logic clk, input logic rst, input logic a, input logic b);
  property p;
    @(posedge clk) disable iff (rst) a |-> b;
  endproperty
  assert property (p);
endmodule
