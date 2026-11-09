// $stable of an unpacked array.
module top(input logic clk, input logic [7:0] a [2]);
  assert property (@(posedge clk) $stable(a));
endmodule
