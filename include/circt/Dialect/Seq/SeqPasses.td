//===-- SeqPasses.td - Seq pass definition file ------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains definitions for passes that work on the Seq dialect.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_SEQ_SEQPASSES
#define CIRCT_DIALECT_SEQ_SEQPASSES

include "mlir/Pass/PassBase.td"

def LowerFirRegToCompReg : Pass<"seq-lower-firreg-to-compreg",
                                "hw::HWModuleOp"> {
  let summary = "Lower seq.firreg to seq.compreg with an initial value";
  let description = [{
    Lowers `seq.firreg` to `seq.compreg` without reset and with an explicit
    constant initial value, for formal backends that only take plain
    registers with an initial state (e.g. AIGER latches).

    A reset becomes a mux on the next value; an asynchronous reset is also
    applied to the value read in the cycle where it is asserted. The
    initial state is, in this order: the reset value with `initial-reset`
    (the run starts in reset; a non-constant reset value is taken in the
    first cycle through a mux on a `first` register), the preset, or any
    value (`verif.symbolic_value`, taken in the first cycle) with
    `free-initial` (firreg registers start randomized), else 0. Aggregate
    registers are lowered on their bits.
  }];
  let options = [
    Option<"initialReset", "initial-reset", "bool", "true",
           "Registers with a reset start in their reset value">,
    Option<"freeInitial", "free-initial", "bool", "true",
           "Other registers without preset start with any value (else 0)">
  ];
  let statistics = [
    Statistic<"numConstReset", "num-const-reset",
              "Registers starting in a constant reset value">,
    Statistic<"numValueReset", "num-value-reset",
              "Registers starting in a non-constant reset value">,
    Statistic<"numPreset", "num-preset", "Registers starting in their preset">,
    Statistic<"numFree", "num-free", "Registers starting with any value">,
    Statistic<"numZero", "num-zero", "Registers starting at 0">
  ];
  let dependentDialects = ["circt::comb::CombDialect", "circt::hw::HWDialect",
                           "circt::verif::VerifDialect"];
}

def LowerSeqCompRegCE : Pass<"lower-seq-compreg-ce", "hw::HWModuleOp"> {
  let summary = "Lower seq.compreg.ce to seq.compreg";
  let description = [{
    Lowers seq.compreg.ce to a seq.compreg with the clock enable signal
    built into the next logic, i.e. `next := mux(clock_enable, next, current)`
  }];
  let dependentDialects = ["circt::comb::CombDialect, circt::hw::HWDialect"];
}

def LowerSeqFIFO : Pass<"lower-seq-fifo", "hw::HWModuleOp"> {
  let summary = "Lower seq.fifo ops";
  let dependentDialects = [
    "circt::hw::HWDialect",
    "circt::comb::CombDialect",
    "circt::verif::VerifDialect"
  ];
}

def LowerSeqHLMem: Pass<"lower-seq-hlmem", "hw::HWModuleOp"> {
  let summary = "Lowers seq.hlmem operations.";
  let dependentDialects = ["circt::sv::SVDialect"];
}

def ExternalizeClockGate: Pass<"externalize-clock-gate", "mlir::ModuleOp"> {
  let summary = "Convert seq.clock_gate ops into hw.module.extern instances";
  let dependentDialects = ["circt::hw::HWDialect", "circt::comb::CombDialect"];
  let options = [
    Option<"moduleName", "name", "std::string", "\"CKG\"",
           "Name of the external clock gate module">,
    Option<"inputName", "input", "std::string", "\"I\"",
           "Name of the clock input">,
    Option<"outputName", "output", "std::string", "\"O\"",
           "Name of the gated clock output">,
    Option<"enableName", "enable", "std::string", "\"E\"",
           "Name of the enable input">,
    Option<"testEnableName", "test-enable", "std::string", "\"TE\"",
           "Name of the optional test enable input">,
    Option<"instName", "instance-name", "std::string", "\"ckg\"",
           "Name of the generated instances">
  ];
  let statistics = [
    Statistic<"numClockGatesConverted", "num-clock-gates-converted",
      "Number of clock gates converted to external module instances">
  ];
}

def ClockGateToEnable : Pass<"seq-clock-gate-to-enable", "hw::HWModuleOp"> {
  let summary = "Clock registers behind seq.clock_gate with the ungated clock";
  let description = [{
    A register clocked by `seq.clock_gate %clk, %en` only takes a value at a
    rising edge of `%clk` where `%en` (or the test enable) is 1. It is clocked
    with `%clk` instead and keeps its value in the other cycles:
    `next := mux(en, next, current)`; a synchronous reset only applies when
    `en` is 1, an asynchronous reset is unchanged. Chains of clock gates are
    combined. This gives one clock domain for backends with a single implicit
    clock (e.g. AIGER), with the same behavior at every edge of `%clk`.

    Applies to `seq.firreg`, `seq.compreg` and `seq.compreg.ce` in the same
    module as the gate; other users of the gated clock keep it.
  }];
  let statistics = [
    Statistic<"numRegisters", "num-registers",
              "Registers moved from a gated clock to a clock enable">,
  ];
  let dependentDialects = ["circt::comb::CombDialect"];
}

def HWMemSimImpl : Pass<"hw-memory-sim", "ModuleOp"> {
  let summary = "Implement FIRRTMMem memories nodes with simulation model";
  let description = [{
    This pass replaces generated module nodes of type FIRRTLMem with a model
    suitable for simulation.
  }];

  let dependentDialects = [
    "circt::emit::EmitDialect",
    "circt::hw::HWDialect",
    "circt::sv::SVDialect",
  ];

  let options = [
    Option<"disableMemRandomization", "disable-mem-randomization", "bool", "false",
            "Disable emission of memory randomization code">,
    Option<"disableRegRandomization", "disable-reg-randomization", "bool", "false",
            "Disable emission of register randomization code">,
    Option<"replSeqMem", "repl-seq-mem", "bool",
                "false", "Prepare seq mems for macro replacement">,
    Option<"readEnableMode", "read-enable-mode", "::circt::seq::ReadEnableMode",
           "::circt::seq::ReadEnableMode::Undefined",
           "specify the behaviour of the read enable signal", [{
              ::llvm::cl::values(
              clEnumValN(::circt::seq::ReadEnableMode::Ignore, "ignore",
                         "Ignore the read enable signal"),
              clEnumValN(::circt::seq::ReadEnableMode::Zero, "zero",
                         "Output zeros when reads are disabled"),
              clEnumValN(::circt::seq::ReadEnableMode::Undefined, "undefined",
                         "Output X when reads are disbled"))
           }]>,
    Option<"addMuxPragmas", "add-mux-pragmas", "bool", "false",
            "Add mux pragmas to memory reads">,
    Option<"addVivadoRAMAddressConflictSynthesisBugWorkaround",
           "add-vivado-ram-address-conflict-synthesis-bug-workaround", "bool", "false",
            "Add a vivado attribute to specify a ram style of an array register">

   ];
}

def LowerSeqShiftReg : Pass<"lower-seq-shiftreg", "hw::HWModuleOp"> {
  let summary = "Lower seq.shiftreg ops";
  let description = [{
    Default pass for lowering shift register operations. This will lower
    into a chain of `seq.compreg.ce` operations.
    Note that this is _not_ guaranteed to be a performant implementation,
    but instead a default, fallback lowering path which is guaranteed to
    provide a semantically valid path to verilog emissions.
    Users are expected to provide a custom lowering pass to maps `seq.shiftreg`
    operations to target-specific primitives.
  }];
  let dependentDialects = ["circt::hw::HWDialect"];
}

def RegOfVecToMem : Pass<"seq-reg-of-vec-to-mem", "hw::HWModuleOp"> {
  let summary = "Convert register arrays to FIRRTL memories";
  let description = [{
    This pass identifies register arrays that follow memory access patterns
    and converts them to seq.firmem operations. It looks for patterns where:
    1. A register array is updated via array_inject operations
    2. The array is read via array_get operations  
    3. Updates are controlled by enable signals through mux operations
    4. Read and write operations use the same clock
  }];
}

def InferClockPorts : Pass<"seq-infer-clock-ports", "mlir::ModuleOp"> {
  let summary = "Infer `!seq.clock` types for `i1` ports used as clocks";
  let description = [{
    Walks backwards from every `seq.to_clock` through the instance hierarchy
    and retypes the `i1` module and instance ports it passes through to
    `!seq.clock`. Input ports are promoted whenever they are used as a clock;
    output ports only if they are driven by a clock, so combinational logic on
    a clock path (e.g. a gated clock) stays `i1`. Remaining non-clock uses of a
    promoted value are rewired through a single `seq.from_clock`.
  }];
  let dependentDialects = [
    "circt::hw::HWDialect",
    "circt::seq::SeqDialect"
  ];
  let options = [
    Option<"promotePublicPorts", "promote-public-ports", "bool", "true",
           "Also retype ports of publicly visible modules">
  ];
  let statistics = [
    Statistic<"numInputPortsPromoted", "num-input-ports-promoted",
      "Number of input ports retyped to !seq.clock">,
    Statistic<"numOutputPortsPromoted", "num-output-ports-promoted",
      "Number of output ports retyped to !seq.clock">,
    Statistic<"numCastsRemoved", "num-casts-removed",
      "Number of seq.to_clock casts eliminated">
  ];
}

def UnrollMemoryLatencies : Pass<"seq-unroll-mem-latency", "hw::HWModuleOp"> {
  let summary = "Unrolls memory port latencies into registers";
  let description = [{
    Converts all memory port latencies, for both `seq.firmem` and `seq.hlmem`,
    into explicit latencies by padding ports with shift registers, such that 
    for a latency of `n`, reads and writes occur `n` cycles after the data 
    was made available. 
    
    This means that if at cycle 10 a read port with latency 2 has address 0 
    and is enabled, then the read data port will yield the data from address 0 
    at cycle 12. This is modeled by padding the read-address port with a 
    shift-register of depth equal to the latency of that port.

    For writes, the default latency is 1, therefore we can pad the write port's 
    data and address inputs with a shift-register of depth n-1.
  }];

  let dependentDialects = ["circt::hw::HWDialect", "circt::comb::CombDialect"];
}

#endif // CIRCT_DIALECT_SEQ_SEQPASSES
