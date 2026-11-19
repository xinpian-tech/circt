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

def TrimRegisters : Pass<"seq-trim-registers", "hw::HWModuleOp"> {
  let summary = "Remove register bits that nothing observes";
  let description = [{
    A cone of influence at the granularity of bits. The observed bits of each
    value are propagated from the module outputs and from the operations that
    observe their operands (assertions, assumptions and anything unknown)
    through extracts, concatenations, replications, bitwise operators, mux
    data inputs, the low bits of adders and multipliers, and through
    registers from their result to their next state and reset value. A
    register is then narrowed to its observed bits; the removed bits read as
    zero, which no observer can tell apart. This removes register bits that
    only feed themselves, which the cone of influence of whole operations
    keeps. Registers with an inner symbol or a non-constant initial value are
    left alone.
  }];
  let dependentDialects = ["circt::hw::HWDialect", "circt::comb::CombDialect"];
  let statistics = [
    Statistic<"numTrimmedRegisters", "num-trimmed-registers",
              "Number of registers narrowed or removed">,
    Statistic<"numRemovedBits", "num-removed-bits",
              "Number of register bits removed">,
  ];
}

def RegisterBisimulation : Pass<"seq-register-bisimulation", "hw::HWModuleOp"> {
  let summary = "Merge registers and logic that are equal by structure";
  let description = [{
    Structural hashing through registers, on words: values are partitioned
    by partition refinement, starting with all registers of one type, clock
    and constant initial value in one class and all operations of one kind
    and attributes in one class, and splitting classes whose members read
    operands of different classes (sorted for commutative operations) until
    nothing splits. The members of a class start equal and compute the same
    function of equal values, so they are equal in every reachable state;
    each class is merged into its first member. No SAT solver is used: this
    finds structural duplicates (such as registers of assertions that mirror
    the design) cheaply, before signal correspondence.

    Registers with an inner symbol or a non-constant initial value, block
    arguments, and operations with side effects, regions or several results
    are classes of their own.
  }];
  let statistics = [
    Statistic<"numMergedRegisters", "num-merged-registers",
              "Registers merged">,
    Statistic<"numMergedOps", "num-merged-ops", "Operations merged">,
    Statistic<"numRounds", "num-rounds", "Refinement rounds">,
  ];
}

def WordCorrespondence : Pass<"seq-word-correspondence", "hw::HWModuleOp"> {
  let summary = "Signal correspondence on words, proved with an SMT solver";
  let description = [{
    Signal correspondence on the word-level IR, before bit-blasting: whole
    values (registers and combinational operations) that are equal to each
    other or constant in every reachable state from the initial state, and
    the bits of a value that are constant, are merged. Candidates come from
    word-level random simulation; they are proved by induction (they hold in
    the initial state, and if all hold in a cycle they hold in the next),
    each check a bit-vector problem written as SMT-LIB from the IR and solved
    by an external solver (`solver`, Bitwuzla by default). A counterexample is
    simulated to split the classes, until none splits.

    Registers on another clock than the most common one, with a reset, an
    inner symbol or a non-constant initial value, and operations other than
    the bitwise, arithmetic (add, sub, mul), comparison, mux, extract,
    concat, replicate and shift operations of comb are free values in each
    cycle. Assumptions are not used.
  }];
  let options = [
    Option<"numPatterns", "num-patterns", "unsigned", "256",
           "Simulated runs from the initial state">,
    Option<"numFrames", "num-frames", "unsigned", "64",
           "Simulated cycles per run">,
    Option<"seed", "seed", "uint64_t", "0", "Seed of the simulation">,
    Option<"maxIterations", "max-iterations", "unsigned", "10000",
           "Checks before giving up (then nothing is merged)">,
    Option<"chunk", "chunk", "unsigned", "32",
           "Classes checked by one SMT query">,
    Option<"solver", "solver", "std::string", "\"bitwuzla\"",
           "SMT solver (reads SMT-LIB from a file argument)">,
    Option<"dumpDir", "dump-dir", "std::string", "\"\"",
           "Debugging: directory to keep the SMT-LIB queries in">
  ];
  let statistics = [
    Statistic<"numCandidates", "num-candidates",
              "Values in candidate classes after simulation">,
    Statistic<"numCandidateBits", "num-candidate-bits",
              "Values with constant bits after simulation">,
    Statistic<"numQueries", "num-queries", "SMT queries">,
    Statistic<"numCounterexamples", "num-counterexamples",
              "Counterexamples">,
    Statistic<"numClasses", "num-classes", "Proved classes">,
    Statistic<"numMergedValues", "num-merged-values", "Values merged">,
    Statistic<"numConstantBits", "num-constant-bits",
              "Bits of values replaced by constants">,
    Statistic<"numCutOps", "num-cut-ops",
              "Operations closing a word-level cycle, made free">,
    Statistic<"msSimulate", "ms-simulate", "Milliseconds simulating">,
    Statistic<"msProve", "ms-prove", "Milliseconds proving">
  ];
}

def KnownBitsInvariants : Pass<"seq-known-bits", "hw::HWModuleOp"> {
  let summary = "Constant bits of registers in all reachable states";
  let description = [{
    Abstract interpretation on words with known bits (llvm::KnownBits):
    registers start at their constant initial values, the logic is evaluated
    on the abstraction with unknown inputs, and next states are joined into
    the registers' abstraction until it no longer changes. Bits still known
    are constant in every reachable state: registers read them as constants,
    and combinational values that are wholly known become constants. No SAT
    solver is used.

    Registers with a reset, an inner symbol or a non-constant initial value
    are not analysed (unknown); so are operations closing a combinational
    cycle and operations other than those of comb.
  }];
  let statistics = [
    Statistic<"numRounds", "num-rounds", "Fixpoint rounds">,
    Statistic<"numRegisterBits", "num-register-bits",
              "Register bits replaced by constants">,
    Statistic<"numConstantValues", "num-constant-values",
              "Combinational values replaced by constants">,
    Statistic<"numConstantBits", "num-constant-bits",
              "Known bits of other combinational values (with partial)">,
  ];
  let options = [
    Option<"partial", "partial", "bool", "false",
           "Also read the known bits of combinational values as constants">,
    Option<"context", "context", "bool", "false",
           "Evaluate the arms of muxes under their conditions">,
    Option<"depth", "depth", "unsigned", "8",
           "Depth of the evaluation under a condition">,
    Option<"budget", "budget", "unsigned", "256",
           "Operations evaluated under a condition per arm">
  ];
  let dependentDialects = ["comb::CombDialect", "hw::HWDialect"];
}

def WordCongruence : Pass<"seq-word-congruence", "hw::HWModuleOp"> {
  let summary = "Equal and constant words through registers, without a solver";
  let description = [{
    A greatest fixpoint of a partition of the registers combined with a least
    fixpoint of their known bits (llvm::KnownBits): the reduced product of a
    relational domain (which registers are equal) and a non-relational one
    (which of their bits are constant). Registers start in classes of the
    same type, clock and constant initial value, with the known bits of that
    value. Each round numbers the values of the logic (global value numbering
    with the registers of a class numbered alike, constant folding with the
    known bits, and rewriting of concatenations, extracts, bitwise
    operations, muxes and comparisons), splits the classes whose members'
    next states get different numbers, and joins the known bits of each
    class with those of its next states. At the fixpoint the partition and
    known bits are an inductive invariant: the registers of a class are
    merged, values of one number are merged, and known bits become constants.
    No SAT or SMT solver is used.
  }];
  let statistics = [
    Statistic<"numRounds", "num-rounds", "Fixpoint rounds">,
    Statistic<"numMergedRegisters", "num-merged-registers",
              "Registers merged into an equal one">,
    Statistic<"numMergedValues", "num-merged-values",
              "Combinational values merged into an equal one">,
    Statistic<"numConstantValues", "num-constant-values",
              "Combinational values replaced by constants">,
    Statistic<"numRegisterBits", "num-register-bits",
              "Register bits replaced by constants">,
    Statistic<"numEqualUnder", "num-equal-under",
              "Next states numbered apart but shown equal by case splits">,
  ];
  let options = [
    Option<"splits", "splits", "unsigned", "4",
           "Mux conditions split on to show next states equal (0: none)">,
    Option<"depth", "depth", "unsigned", "8", "Depth of the evaluation">,
    Option<"budget", "budget", "unsigned", "256",
           "Operations evaluated per comparison">
  ];
  let dependentDialects = ["comb::CombDialect", "hw::HWDialect"];
}

def ContextSimplify : Pass<"comb-context-simplify", "hw::HWModuleOp"> {
  let summary = "Simplify logic in the context where it matters, on words";
  let description = [{
    A conjunct of an i1 and is evaluated on known bits assuming the other
    conjuncts hold (an equality with a constant makes the compared word
    known), a disjunct of an or assuming the others do not, an arm of a mux
    assuming its condition selects it. Conjuncts known 1 are dropped, known 0
    make the and 0 (dually for or), arms wholly known become constants.
    Negated comparisons with constants are kept as disequalities (passed
    through bijections with constants), disjunctions among the facts are
    split into cases. In an arm, muxes with a condition known there are their
    arm and operands of an or known 0 there are dropped; the bits of a mux
    where one arm equals the other whenever the other is selected (by known
    bits, or slice by slice with case splits) are that arm. With `or-form`,
    a mux whose arms are disjoint becomes the or of its arms. (In the Seq
    library for the known-bits transfer functions.)
  }];
  let options = [
    Option<"depth", "depth", "unsigned", "8", "Depth of the evaluation">,
    Option<"budget", "budget", "unsigned", "256",
           "Operations evaluated per operand">,
    Option<"splits", "splits", "unsigned", "4",
           "Depth of the case splits (on mux conditions, or on the operands "
           "of a disjunction in the facts) to show an arm equals the other">,
    Option<"steps", "steps", "unsigned", "16",
           "Case splits per arm shown equal to the other">,
    Option<"cases", "cases", "unsigned", "1",
           "Cases (of splits on disjunctions in the facts) an arm is "
           "evaluated in">,
    Option<"orForm", "or-form", "bool", "false",
           "Turn a mux whose arms are disjoint (bits of one arm 0 whenever "
           "the other is selected) into the or of its arms, gated by the "
           "condition only where needed">
  ];
  let statistics = [
    Statistic<"numDropped", "num-dropped", "Operands dropped">,
    Statistic<"numFolded", "num-folded", "and/or replaced by a constant">,
    Statistic<"numArms", "num-arms", "Mux arms replaced by a constant">,
    Statistic<"numRedundant", "num-redundant",
              "Muxes (or some of their bits) equal to one arm">,
  ];
  let dependentDialects = ["comb::CombDialect", "hw::HWDialect"];
}

#endif // CIRCT_DIALECT_SEQ_SEQPASSES
