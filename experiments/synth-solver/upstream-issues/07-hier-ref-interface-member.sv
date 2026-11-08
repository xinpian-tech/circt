// A hierarchical reference into a sibling subtree that names a member of
// an interface instance.
interface bus_if;
  logic valid;
endinterface

module core(input logic clk, input logic d);
  bus_if bus();
  always_ff @(posedge clk) bus.valid <= d;
endmodule

module checker_m(input logic clk, input logic v);
  assert property (@(posedge clk) v == v);
endmodule

module top(input logic clk, input logic d);
  core u_core(.clk(clk), .d(d));
  checker_m chk(.clk(clk), .v(u_core.bus.valid));
endmodule
