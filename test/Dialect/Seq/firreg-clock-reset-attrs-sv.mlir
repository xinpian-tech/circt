// Default (plain `always`) SystemVerilog emission of the seq.firreg clock-edge,
// reset-type, and reset-polarity attributes, verified on the EMITTED Verilog
// after CSE -- mirroring firtool, which runs CSE after lower-seq-to-sv. This is
// the end-to-end golden for every clock-edge x reset-type x reset-polarity
// combination (rows 1-10). It complements firreg-clock-reset-attrs.mlir (which
// checks the sv.always lowering at the MLIR level) and firreg-always-ff.mlir
// (the opt-in always_ff mode).
//
// The behavior-changing cases are the active-low async resets (rows 4 and 8):
// they trigger on the configured edge of the ORIGINAL reset signal (`negedge
// rst`) and test the inline `~rst` condition, with NO manufactured inverted
// temporary (`wire _GEN = ~rst`). CSE merges the always-if and initial-block
// `~rst` uses into one multi-use value; ExportVerilog's isDuplicatableExpression
// duplicates the 1-bit NOT-of-leaf inline at each use so it is never spilled to
// a wire that would race against the raw-`rst` sensitivity edge.
//
// RUN: circt-opt %s --lower-seq-to-sv="disable-reg-randomization" --cse --export-verilog | FileCheck %s --check-prefix=SV

// Row 1: posedge clock, sync active-high reset.
// SV-LABEL: module Row1_PosSyncHigh
// SV: always @(posedge clk) begin
// SV-NEXT: if (rst)
// SV-NEXT: r <= 32'h0;
// SV: else
// SV-NEXT: r <= d;
hw.module @Row1_PosSyncHigh(in %clk:!seq.clock, in %rst:i1, in %d:i32, out q:i32) {
  %c0 = hw.constant 0 : i32
  %r = seq.firreg %d clock %clk reset sync %rst, %c0 {clockEdge = 0 : i32, resetPolarity = 0 : i32} : i32
  hw.output %r : i32
}

// Row 2: posedge clock, sync active-low reset -> inline `if (~rst)`, no `_GEN`.
// SV-LABEL: module Row2_PosSyncLow
// SV-NOT: = ~rst
// SV: always @(posedge clk) begin
// SV-NEXT: if (~rst)
// SV-NEXT: r <= 32'h0;
hw.module @Row2_PosSyncLow(in %clk:!seq.clock, in %rst:i1, in %d:i32, out q:i32) {
  %c0 = hw.constant 0 : i32
  %r = seq.firreg %d clock %clk reset sync %rst, %c0 {clockEdge = 0 : i32, resetPolarity = 1 : i32} : i32
  hw.output %r : i32
}

// Row 3: posedge clock, async active-high reset.
// SV-LABEL: module Row3_PosAsyncHigh
// SV: always @(posedge clk or posedge rst) begin
// SV-NEXT: if (rst)
// SV-NEXT: r <= 32'h0;
hw.module @Row3_PosAsyncHigh(in %clk:!seq.clock, in %rst:i1, in %d:i32, out q:i32) {
  %c0 = hw.constant 0 : i32
  %r = seq.firreg %d clock %clk reset async %rst, %c0 {clockEdge = 0 : i32, resetPolarity = 0 : i32} : i32
  hw.output %r : i32
}

// Row 4: posedge clock, async active-low reset (behavior-changing): `negedge rst`
// on the original signal, inline `if (~rst)`, no inverted temporary.
// SV-LABEL: module Row4_PosAsyncLow
// SV-NOT: = ~rst
// SV: always @(posedge clk or negedge rst) begin
// SV-NEXT: if (~rst)
// SV-NEXT: r <= 32'h0;
// SV: else
// SV-NEXT: r <= d;
hw.module @Row4_PosAsyncLow(in %clk:!seq.clock, in %rst:i1, in %d:i32, out q:i32) {
  %c0 = hw.constant 0 : i32
  %r = seq.firreg %d clock %clk reset async %rst, %c0 {clockEdge = 0 : i32, resetPolarity = 1 : i32} : i32
  hw.output %r : i32
}

// Row 5: negedge clock, sync active-high reset.
// SV-LABEL: module Row5_NegSyncHigh
// SV: always @(negedge clk) begin
// SV-NEXT: if (rst)
hw.module @Row5_NegSyncHigh(in %clk:!seq.clock, in %rst:i1, in %d:i32, out q:i32) {
  %c0 = hw.constant 0 : i32
  %r = seq.firreg %d clock %clk reset sync %rst, %c0 {clockEdge = 1 : i32, resetPolarity = 0 : i32} : i32
  hw.output %r : i32
}

// Row 6: negedge clock, sync active-low reset -> inline `if (~rst)`, no `_GEN`.
// SV-LABEL: module Row6_NegSyncLow
// SV-NOT: = ~rst
// SV: always @(negedge clk) begin
// SV-NEXT: if (~rst)
hw.module @Row6_NegSyncLow(in %clk:!seq.clock, in %rst:i1, in %d:i32, out q:i32) {
  %c0 = hw.constant 0 : i32
  %r = seq.firreg %d clock %clk reset sync %rst, %c0 {clockEdge = 1 : i32, resetPolarity = 1 : i32} : i32
  hw.output %r : i32
}

// Row 7: negedge clock, async active-high reset.
// SV-LABEL: module Row7_NegAsyncHigh
// SV: always @(negedge clk or posedge rst) begin
// SV-NEXT: if (rst)
hw.module @Row7_NegAsyncHigh(in %clk:!seq.clock, in %rst:i1, in %d:i32, out q:i32) {
  %c0 = hw.constant 0 : i32
  %r = seq.firreg %d clock %clk reset async %rst, %c0 {clockEdge = 1 : i32, resetPolarity = 0 : i32} : i32
  hw.output %r : i32
}

// Row 8: negedge clock, async active-low reset (behavior-changing): `negedge rst`
// on the original signal, inline `if (~rst)`, no inverted temporary.
// SV-LABEL: module Row8_NegAsyncLow
// SV-NOT: = ~rst
// SV: always @(negedge clk or negedge rst) begin
// SV-NEXT: if (~rst)
hw.module @Row8_NegAsyncLow(in %clk:!seq.clock, in %rst:i1, in %d:i32, out q:i32) {
  %c0 = hw.constant 0 : i32
  %r = seq.firreg %d clock %clk reset async %rst, %c0 {clockEdge = 1 : i32, resetPolarity = 1 : i32} : i32
  hw.output %r : i32
}

// Row 9: posedge clock, no reset.
// SV-LABEL: module Row9_PosNoReset
// SV: always @(posedge clk)
// SV-NEXT: r <= d;
hw.module @Row9_PosNoReset(in %clk:!seq.clock, in %d:i32, out q:i32) {
  %r = seq.firreg %d clock %clk {clockEdge = 0 : i32} : i32
  hw.output %r : i32
}

// Row 10: negedge clock, no reset.
// SV-LABEL: module Row10_NegNoReset
// SV: always @(negedge clk)
// SV-NEXT: r <= d;
hw.module @Row10_NegNoReset(in %clk:!seq.clock, in %d:i32, out q:i32) {
  %r = seq.firreg %d clock %clk {clockEdge = 1 : i32} : i32
  hw.output %r : i32
}
