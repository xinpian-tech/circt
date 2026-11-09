// Environment of a stand-alone ex_stage (what the ID stage guarantees):
// id_ex_pipe is a pipeline register that only moves when EX is ready
// (cv32e40x_id_stage.sv: the id_ex_pipe_o updates are under ex_ready_i).
// Written with ##1 instead of |=>: upstream circt-verilog encodes a |=> b
// as |-> (upstream-issues/README.md, issue 1; fixed on this branch).
module cv32e40x_ex_stage_env_sva
  import cv32e40x_pkg::*;
(
  input logic        clk,
  input logic        rst_n,
  input logic        ex_ready_o,
  input id_ex_pipe_t id_ex_pipe_i
);
  env_id_ex_pipe_held :
    assume property (@(posedge clk) disable iff (!rst_n)
                     !ex_ready_o ##1 1'b1 |-> $stable(id_ex_pipe_i));
endmodule
