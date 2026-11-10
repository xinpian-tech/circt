// A hierarchical reference from one subtree into a sibling subtree:
// `u_ctrl.chk` reads `top.u_core.state`.
module core(input logic clk, input logic d);
  logic state;
  always_ff @(posedge clk) state <= d;
endmodule

module checker_m(input logic clk, input logic s);
  assert property (@(posedge clk) s == s);
endmodule

module ctrl(input logic clk);
  checker_m chk(.clk(clk), .s(top.u_core.state));
endmodule

module top(input logic clk, input logic d);
  core u_core(.clk(clk), .d(d));
  ctrl u_ctrl(.clk(clk));
endmodule
