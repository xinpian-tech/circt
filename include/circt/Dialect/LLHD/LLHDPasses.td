//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_LLHD_LLHDPASSES_TD
#define CIRCT_DIALECT_LLHD_LLHDPASSES_TD

include "mlir/Pass/PassBase.td"

def InlineCallsPass : Pass<"llhd-inline-calls", "ModuleOp"> {
  let summary = "Inline all function calls in HW modules";
  let description = [{
    Inlines `func.call` operations nested within `llhd.combinational`,
    `llhd.process`, and `llhd.final` ops within `hw.module`s. The `func.func`
    definitions are left untouched. After the inlining, MLIR's symbol DCE pass
    may be used to eliminate the function definitions where possible. Recursive
    calls are left unchanged.

    This pass expects the `llhd-wrap-procedural-ops` pass to have already run.
    Otherwise call ops immediately in the module body, a graph region, cannot be
    inlined at all since the graph region cannot accommodate the function's
    control flow.
  }];
  let statistics = [
    Statistic<
      "numInlined", "calls-inlined", "Number of call ops that were inlined"
    >,
  ];
}

def Sig2RegPass : Pass<"llhd-sig2reg", "hw::HWModuleOp"> {
  let summary = "Promote LLHD signals to SSA values.";
  let description = [{
    Replace probes of singly-driven signals with the driven value, and remove
    unprobed signals.

    This pass assumes that CombineDrives has already done the heavy lifting of
    combining multiple partial drives into single whole-signal drives.
  }];
  let dependentDialects = ["circt::hw::HWDialect"];
  let statistics = [
    Statistic<"numPromoted", "promoted",
      "Number of signals promoted to values">,
    Statistic<"numUnused", "unused-removed",
      "Number of removed signals because they were unprobed">,
    Statistic<"numSkipped", "unpromotable",
      "Number of signals that could not be promoted">,
  ];
}

def RaiseClockGatesPass : Pass<"llhd-raise-clock-gates", "hw::HWModuleOp"> {
  let summary = "Replace latch-based clock gates with seq.clock_gate";
  let description = [{
    Recognizes the usual model of a clock gate cell, a latch that is
    transparent while the clock is low followed by an AND with the clock:

    ```verilog
    always_latch if (!clk) en_q = en;
    assign gclk = clk & en_q;
    ```

    which the import turns into a signal driven only while the clock is low,
    `llhd.drv %en_q, %en after ... if !clk`, probed in `comb.and %clk, %p`.
    The AND is replaced with `seq.clock_gate %clk, %e` (through
    `seq.from_clock` where it is used as a value, e.g. as the clock of an
    assertion), where `%e` is the driven value with the clock set to 0: the
    value the latch holds at the rising edge of the clock, which is when
    `seq.clock_gate` samples its enable. The latch signal is left in place
    for any other users; it is removed once unused.
  }];
  let dependentDialects = ["circt::seq::SeqDialect", "circt::hw::HWDialect"];
  let statistics = [
    Statistic<"numRaised", "raised", "Number of clock gates raised">,
  ];
}

def Mem2RegPass : Pass<"llhd-mem2reg"> {
  let summary = "Promotes memory and signal slots into values.";
  let dependentDialects = [
    "comb::CombDialect",
    "hw::HWDialect",
    "mlir::cf::ControlFlowDialect",
  ];
}

def HoistSignalsPass : Pass<"llhd-hoist-signals"> {
  let summary = "Hoist probes and promote drives to process results";
  let dependentDialects = ["mlir::arith::ArithDialect"];
}

def LowerProcessesPass : Pass<"llhd-lower-processes", "hw::HWModuleOp"> {
  let summary = "Convert process ops to combinational ops where possible";
}

def DeseqPass : Pass<"llhd-deseq", "hw::HWModuleOp"> {
  let summary = "Convert sequential processes to registers";
  let dependentDialects = [
    "comb::CombDialect",
    "hw::HWDialect",
    "seq::SeqDialect",
  ];
}

def CombineDrivesPass : Pass<"llhd-combine-drives", "hw::HWModuleOp"> {
  let summary = "Combine scalar drives into aggregate drives";
  let description = [{
    If individual drives cover all of an aggregate signal's fields, merge them
    into a single drive of the whole aggregate value.

    Undriven parts take the signal's initial value, or any value
    (`verif.symbolic_value`) with `free-undriven` if it has none. A drive of
    the whole signal besides drives of parts of it is merged too if its value
    equals the probed signal on those parts (a read-modify-write): it then
    provides the remaining parts.
  }];
  let options = [
    Option<"freeUndriven", "free-undriven", "bool", "false",
           "Fill undriven parts of signals without initial value with any "
           "value">
  ];
  let dependentDialects = [
    "comb::CombDialect",
    "hw::HWDialect",
    "verif::VerifDialect",
  ];
}

def WrapProceduralOpsPass : Pass<"llhd-wrap-procedural-ops", "hw::HWModuleOp"> {
  let summary = "Wrap procedural ops in modules to make them inlinable";
  let description = [{
    Operations such as `func.call` or `scf.if` may appear in an `hw.module` body
    directly. They cannot be inlined into the module though, since the inlined
    function or result of converting the SCF ops to the CF dialect may create
    control-flow operations with multiple blocks. This pass wraps such
    operations in `llhd.combinational` ops to give them an SSACFG region to
    inline into.
  }];
  let dependentDialects = [
    "llhd::LLHDDialect",
  ];
  let statistics = [
    Statistic<
      "numOpsWrapped", "ops-wrapped", "Number of procedural ops wrapped"
    >,
  ];
}

def RemoveControlFlowPass : Pass<"llhd-remove-control-flow", "hw::HWModuleOp"> {
  let summary = "Remove acyclic control flow and replace block args with muxes";
  let description = [{
    Remove the control flow in `llhd.combinational` operations by merging all
    blocks into the entry block and replacing block arguments with multiplexers.
    This requires the control flow to be acyclic, for example by unrolling all
    loops beforehand. Additionally, since this moves operations from
    conditionally executed blocks into the unconditionally executed entry block,
    all operations must be side-effect free.
  }];
  let dependentDialects = [
    "comb::CombDialect",
    "hw::HWDialect",
    "mlir::cf::ControlFlowDialect",
  ];
}

def UnrollLoopsPass : Pass<"llhd-unroll-loops", "hw::HWModuleOp"> {
  let summary = "Unroll control flow loops with static bounds";
  let description = [{
    Unroll loops in `llhd.combinational` operations by replicating the loop body
    and replacing induction variables with constants. The loop bounds must be
    known at compile time.
  }];
}

#endif // CIRCT_DIALECT_LLHD_LLHDPASSES_TD
