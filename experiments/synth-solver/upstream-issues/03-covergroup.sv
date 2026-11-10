// A covergroup declared in a module (not instantiated).
module top(input logic clk, input logic [1:0] x);
  covergroup cg @(posedge clk);
    coverpoint x;
  endgroup
endmodule
