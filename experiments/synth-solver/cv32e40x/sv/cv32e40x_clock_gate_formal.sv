// Formal model of bhv/cv32e40x_sim_clock_gate.sv with the gate open
// (cv32e40x_core_top.sv ties scan_cg_en_i to 1, so the latch holds 1 and
// clk_o = clk_i). Avoids the level-sensitive latch.
module cv32e40x_clock_gate
#(
  parameter LIB = 0
  )
(
    input  logic clk_i,
    input  logic en_i,
    input  logic scan_cg_en_i,
    output logic clk_o
  );
  assign clk_o = clk_i;
endmodule
