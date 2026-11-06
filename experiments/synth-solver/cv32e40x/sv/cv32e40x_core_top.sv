// Top for formal: cv32e40x_wrapper (all SVA bound) with the eXtension
// interface instantiated inside (X_EXT = 0: no coprocessor) and the clock
// gate kept open.
// Generated from bhv/cv32e40x_wrapper.sv; regenerate if its ports change.
module cv32e40x_core_top #(parameter int DBG_NUM_TRIGGERS = 1) (
  input  logic        clk_i,
  input  logic        rst_ni,
  input  logic [31:0] boot_addr_i,
  input  logic [31:0] mtvec_addr_i,
  input  logic [31:0] dm_halt_addr_i,
  input  logic [31:0] mhartid_i,
  input  logic  [3:0] mimpid_patch_i,
  input  logic [31:0] dm_exception_addr_i,
  output logic        instr_req_o,
  input  logic        instr_gnt_i,
  input  logic        instr_rvalid_i,
  output logic [31:0] instr_addr_o,
  output logic [1:0]  instr_memtype_o,
  output logic [2:0]  instr_prot_o,
  output logic        instr_dbg_o,
  input  logic [31:0] instr_rdata_i,
  input  logic        instr_err_i,
  output logic        data_req_o,
  input  logic        data_gnt_i,
  input  logic        data_rvalid_i,
  output logic        data_we_o,
  output logic [3:0]  data_be_o,
  output logic [31:0] data_addr_o,
  output logic [1:0]  data_memtype_o,
  output logic [2:0]  data_prot_o,
  output logic        data_dbg_o,
  output logic [31:0] data_wdata_o,
  input  logic [31:0] data_rdata_i,
  input  logic        data_err_i,
  output logic [5:0]  data_atop_o,
  input  logic        data_exokay_i,
  output logic [63:0] mcycle_o,
  input  logic [63:0] time_i,
  input  logic [31:0] irq_i,
  input  logic        wu_wfe_i,
  input  logic                       clic_irq_i,
  input  logic [4:0]   clic_irq_id_i,
  input  logic [ 7:0]                clic_irq_level_i,
  input  logic [ 1:0]                clic_irq_priv_i,
  input  logic                       clic_irq_shv_i,
  output logic        fencei_flush_req_o,
  input logic         fencei_flush_ack_i,
  input  logic        debug_req_i,
  output logic        debug_havereset_o,
  output logic        debug_running_o,
  output logic        debug_halted_o,
  output logic        debug_pc_valid_o,
  output logic [31:0] debug_pc_o,
  input  logic        fetch_enable_i,
  output logic        core_sleep_o
);
  // The clock gate of the sleep unit is a latch; keep it open (the core clock
  // runs in every cycle), which approximates sleep.
  logic scan_cg_en_i;
  assign scan_cg_en_i = 1'b1;
  cv32e40x_if_xif xif ();
  // No coprocessor: its outputs are 0.
  assign xif.compressed_ready = 1'b0;
  assign xif.compressed_resp = '0;
  assign xif.issue_ready = 1'b0;
  assign xif.issue_resp = '0;
  assign xif.mem_valid = 1'b0;
  assign xif.mem_req = '0;
  assign xif.result_valid = 1'b0;
  assign xif.result = '0;
  cv32e40x_wrapper #(.CORE_LOG_ENABLE(0), .DBG_NUM_TRIGGERS(DBG_NUM_TRIGGERS)) wrapper_i (
    .xif_compressed_if (xif),
    .xif_issue_if (xif),
    .xif_commit_if (xif),
    .xif_mem_if (xif),
    .xif_mem_result_if (xif),
    .xif_result_if (xif),
    .*);
endmodule
