// A dynamic index into a constant unpacked array of packed structs.
typedef struct packed {
  logic [7:0] lo;
  logic b;
} cfg_t;

module top(input logic idx, output cfg_t o);
  localparam cfg_t CFG [2] = '{'{lo: 8'h1, b: 1'b0}, '{lo: 8'h2, b: 1'b1}};
  assign o = CFG[idx];
endmodule
