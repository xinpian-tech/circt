// RUN: circt-translate --import-verilog %s | FileCheck %s
// RUN: circt-verilog --ir-moore %s | FileCheck %s --check-prefix=RESOLVED
// REQUIRES: slang

// Internal issue in Slang v3 about jump depending on uninitialised value.
// UNSUPPORTED: valgrind

// Hierarchical names into other instances become `moore.xmr` ops: up to the
// common ancestor, down by instance name, and the name of the target there.
// `moore-resolve-xmr` (first pass of circt-verilog) replaces them by ports.

interface Bus;
  logic valid;
endinterface

module Core(input logic clk, input logic d);
  logic state;
  always_ff @(posedge clk) state <= d;
  Bus bus();
  assign bus.valid = d;
  for (genvar i = 0; i < 2; ++i) begin : gen
    logic g;
    assign g = d;
  end
endmodule

module Checker(input logic clk, input logic s);
endmodule

// A reference into a sibling subtree (here in a port connection).
// CHECK-LABEL: moore.module private @Ctrl
// CHECK: moore.xmr up 1 path ["u_core"] target "state" : <l1>
// CHECK: moore.instance "chk" @Checker
// RESOLVED-LABEL: moore.module private @Ctrl(in %clk : !moore.l1, in %u_core.state : !moore.ref<l1>)
module Ctrl(input logic clk);
  Checker chk(.clk(clk), .s(top.u_core.state));
endmodule

// CHECK-LABEL: moore.module @top
// CHECK: moore.xmr up 0 path ["u_core"] target "bus_valid" : <l1>
// CHECK: moore.xmr up 0 path ["u_core"] target "gen_1.g" : <l1>
// CHECK: moore.xmr up 0 path ["u_arr_1"] target "state" : <l1>
// RESOLVED-LABEL: moore.module @top
// RESOLVED: %u_core.state, %u_core.bus_valid, %u_core.gen_1.g = moore.instance "u_core" @Core
// RESOLVED: moore.instance "u_ctrl" @Ctrl(clk: {{.*}}, u_core.state: %u_core.state: !moore.ref<l1>)
// RESOLVED-NOT: moore.xmr
module top(input logic clk, input logic d);
  Core u_core(.clk(clk), .d(d));
  Ctrl u_ctrl(.clk(clk));
  Core u_arr [2] (.clk(clk), .d(d));
  logic a, b, c;
  // A member of an interface instance, flattened into `Core`.
  assign a = u_core.bus.valid;
  // A variable in a generate block.
  assign b = u_core.gen[1].g;
  // An element of an instance array.
  assign c = u_arr[1].state;
endmodule

// An interface port connected to an interface instance of another module.
interface Obi;
  logic req;
  logic gnt;
  modport monitor(input req, input gnt);
endinterface
module ObiCore(input logic clk);
  Obi bus();
  assign bus.req = clk;
endmodule
module ObiMon(Obi.monitor m, output logic o);
  assign o = m.req & m.gnt;
endmodule
// CHECK-LABEL: moore.module @ObiTop
// CHECK: moore.xmr up 0 path ["core_i"] target "bus_req" : <l1>
// CHECK: moore.xmr up 0 path ["core_i"] target "bus_gnt" : <l1>
// CHECK: moore.instance "mon_i" @ObiMon
// RESOLVED-LABEL: moore.module @ObiTop
// RESOLVED: %core_i.bus_req, %core_i.bus_gnt = moore.instance "core_i" @ObiCore
module ObiTop(input logic clk, output logic o);
  ObiCore core_i(.clk(clk));
  ObiMon mon_i(.m(core_i.bus), .o(o));
endmodule
