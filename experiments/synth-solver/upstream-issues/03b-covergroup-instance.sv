// A covergroup declared and instantiated in a module.
module top(input logic clk, input logic [1:0] x);
  covergroup cg @(posedge clk);
    coverpoint x;
  endgroup
  cg cg_i = new();
endmodule
