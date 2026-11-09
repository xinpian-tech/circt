//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains definitions for passes related to Synthesis.
//
//===----------------------------------------------------------------------===//


#ifndef CIRCT_DIALECT_SYNTH_TRANSFORMS_PASSES_TD
#define CIRCT_DIALECT_SYNTH_TRANSFORMS_PASSES_TD

include "mlir/Pass/PassBase.td"

def TestPriorityCuts : Pass<"synth-test-priority-cuts", "hw::HWModuleOp"> {
  let summary = "Test priority cuts for synthesis";
  let options = [
    Option<"maxCutsPerRoot", "max-cuts-per-root", "int", "6",
           "Maximum number of cuts to maintain per node">,
    Option<"maxCutInputSize", "max-cut-input-size", "int", "6",
           "Maximum number of cut inputs to consider">,
  ];
}

class CutRewriterPassBase<string name, string op> : Pass<name, op> {
  list<Option> baseOptions = [
    Option<"maxCutsPerRoot", "max-cuts-per-root", "int", "6",
           "Maximum number of cuts to maintain per node">,
    Option<"strategy", "strategy", "synth::OptimizationStrategy",
           /*default=*/"synth::OptimizationStrategyTiming",
           "Optimization strategy (area vs. timing)",
           [{::llvm::cl::values(
             clEnumValN(synth::OptimizationStrategyArea, "area",
                        "Optimize for area"),
             clEnumValN(synth::OptimizationStrategyTiming, "timing",
                        "Optimize for timing")
           )}]>,
    Option<"test", "test", "bool", "false", "Attach timing to IR for testing">
  ];
  let statistics = [
    Statistic<"numCutsCreated", "cuts-created",
              "Number of cuts created during cut enumeration">,
    Statistic<"numCutSetsCreated", "cut-sets-created",
              "Number of cut sets created during cut enumeration">,
    Statistic<"numCutsRewritten", "cuts-rewritten",
              "Number of cuts rewritten">,
  ];
}

def TechMapper : CutRewriterPassBase<"synth-tech-mapper", "mlir::ModuleOp"> {
  let summary = "Technology mapping using cut rewriting";
  let description = [{
    This pass performs technology mapping by converting logic network
    (AIG etc) representations into technology-specific gate implementations.
    It uses cut-based rewriting with priority cuts and NPN canonical forms for
    efficient pattern matching.

    The pass serves dual purposes: providing practical technology mapping
    capabilities and acting as a test vehicle for the cut rewriting framework,
    since testing cut enumeration and pattern matching algorithms directly
    would otherwise be difficult without a concrete application.

    Supports both area and timing optimization strategies.
  }];
  let options = baseOptions;
  let dependentDialects = ["hw::HWDialect"];
}

def GenericLutMapper : CutRewriterPassBase<"synth-generic-lut-mapper",
                                           "hw::HWModuleOp"> {
  let summary = "LUT mapping using generic K-input LUTs";
  let description = [{
    This pass performs technology mapping using generic K-input lookup tables
    (LUTs). It converts combinational logic networks into implementations
    using K-input LUTs (comb.truth_table) with unit area cost and delay.
  }];
  let options = baseOptions # [
    Option<"maxLutSize", "max-lut-size", "unsigned", /*default=*/"6",
           "Maximum number of inputs per LUT">
  ];
  let dependentDialects = ["comb::CombDialect"];
}

def LowerVariadic : Pass<"synth-lower-variadic"> {
  let summary = "Lower variadic operations to binary operations";
  let description = [{
    This pass lowers variadic operations to binary operations using a
    delay-aware algorithm. For commutative operations, it builds a balanced
    tree by combining values with the earliest arrival times first to minimize
    the critical path.
  }];
  let options = [
    ListOption<"opNames", "op-names", "std::string",
               "Specify operation names to lower (empty means all)">,
    Option<"timingAware", "timing-aware", "bool", "true",
           "Lower operators with timing information">,
    Option<"reuseSubsets", "reuse-subsets", "bool", /*default=*/"false",
           "Reuse existing logic subsets to minimize area">
  ];
  let dependentDialects = [
    "circt::comb::CombDialect", "circt::hw::HWDialect",
    "circt::synth::SynthDialect"
  ];
}

def LowerWordToBits : Pass<"synth-lower-word-to-bits"> {
  let summary = "Lower multi-bit AndInverter to single-bit ones";
  let dependentDialects = ["comb::CombDialect"];
  let statistics = [
    Statistic<"numLoweredBits", "num-lowered-bits",
              "Number of total bits lowered including constant">,
    Statistic<"numLoweredConstants", "num-lowered-constants",
              "Number of total constant bits lowered">,
    Statistic<"numLoweredOps", "num-lowered-ops",
              "Number of total operations lowered">,
  ];
}

def PrintLongestPathAnalysis
    : Pass<"synth-print-longest-path-analysis", "mlir::ModuleOp"> {
  let summary =
      "Print longest path analysis results with detailed timing statistics";
  let description = [{
    This pass performs longest path analysis on AIG circuits and outputs detailed
    timing information including:
    - Delay distribution statistics showing timing levels and path counts
    - Critical path details for the top N end points
    - Path history with intermediate debug points for detailed analysis

    The analysis considers each AIG and-inverter operation to have unit delay and
    computes maximum delays through combinational paths across module hierarchies.
  }];
  let options = [Option<
                     "outputFile", "output-file", "std::string", "\"-\"",
                     "Output file for analysis results (use '-' for stdout)">,
                 Option<"test", "test", "bool", "false",
                        "Emit longest paths as diagnostic remarks for testing">,
                 Option<"showTopKPercent", "show-top-k-percent", "int", "5",
                        "The size of the longest paths to show.">,
                 Option<"emitJSON", "emit-json", "bool", "false",
                        "Output analysis results in JSON format">,
                 Option<"topModuleName", "top-module-name", "std::string", "",
                        "Name of the top module to analyze (empty for automatic "
                        "inference from instance graph)">];
  let dependentDialects = ["circt::comb::CombDialect", "circt::hw::HWDialect",
                           "circt::synth::SynthDialect"];
}

def PrintResourceUsageAnalysis
    : Pass<"synth-print-resource-usage-analysis", "mlir::ModuleOp"> {
  let summary = "Print resource usage analysis results";
  let description = [{
    This pass performs resource usage analysis on circuits containing Synth,
    Comb, Seq, and HW dialect operations, and outputs resource utilization
    information.

    The analysis computes both local and total resource usage across module
    hierarchies. Local usage counts resources directly in a module, while total
    usage includes all resources from child module instances.

    JSON format (--emit-json):
      [
        {
          "moduleName": "<module_name>",
          "local": {
            "<operation_name>": <count>,
            ...
          },
          "total": {
            "<operation_name>": <count>,
            ...
          },
          "instances": [
            {
              "instanceName": "<instance_name>",
              "moduleName": "<child_module_name>",
              "usage": {
                "moduleName": "<child_module_name>",
                "local": { ... },
                "total": { ... },
                "instances": [ ... ]
              }
            },
            ...
          ]
        },
        ...
      ]

    The JSON schema is recursive: each module's "instances" array contains
    objects with a "usage" field that has the same structure as the top-level
    module object (moduleName, local, total, instances). This allows the full
    module hierarchy to be represented.

    - "local": Resources used directly in this module (not including children)
    - "total": Resources including all child instances (sum of local + children)
    - "instances": Array of child module instances with their full usage data

    When --top-module-name is empty, the pass analyzes all inferred top-level
    modules and outputs results for each in the top-level JSON array.
  }];
  let options = [Option<"outputFile", "output-file", "std::string", "\"-\"",
                        "Output file for analysis results (use '-' for stdout)">,
                 Option<"emitJSON", "emit-json", "bool", "false",
                        "Output analysis results in JSON format">,
                 Option<"topModuleName", "top-module-name", "std::string", "",
                        "Name of the top module to analyze">];
}

class ExternalSolverPass<string name> : Pass<name, "hw::HWModuleOp"> {
  list<Option> baseOptions = [Option<"continueOnFailure", "continue-on-failure",
                                     "bool", "false",
                                     "Don't fail even if the AIGER exporter, "
                                     "external solver, or AIGER importer fail">,
  ];

  let dependentDialects = ["circt::comb::CombDialect", "circt::hw::HWDialect",
                           "circt::seq::SeqDialect", "circt::verif::VerifDialect"];
}

def AIGERRunner : ExternalSolverPass<"synth-aiger-runner"> {
  let summary = "Run external solver on AIGER files";
  let description = [{
    This pass runs an external solver on AIGER files. It exports the current
    module to AIGER format, runs the external solver, and imports the result
    back into the module.
  }];
  let options =
      baseOptions#[Option<"aigerPath", "aiger-path", "std::string", "",
                          "Path to the AIGER file">,
                   Option<"solverPath", "solver-path", "std::string", "",
                          "Path to the external solver">,
                   ListOption<"solverArgs", "solver-args", "std::string", "">,
  ];
}

def ABCRunner : ExternalSolverPass<"synth-abc-runner"> {
  let summary = "Run ABC on AIGER files";
  let description = [{
    This pass runs ABC on AIGER files. It is a wrapper around AIGERRunner that
    uses ABC as the external solver. It runs the following ABC commands:
    - `read <inputFile>`: Read the AIGER file
    - for each command in `abcCommands`, run `-q <command>`
    - `write <outputFile>`: Write the AIGER file
  }];
  let options =
      baseOptions#[Option<"abcPath", "abc-path", "std::string", "",
                          "Path to the ABC executable">,
                   ListOption<"abcCommands", "abc-commands", "std::string", "">,
  ];
}

def StructuralHash : Pass<"synth-structural-hash"> {
  let summary = "Structural hashing (CSE) for Synth operations";
  let description = [{
    This pass performs aggressive structural hashing-based CSE for Synth dialect
    operations, domain-specific to AIG operations to enable operand
    reordering based on structural properties and inversion flag consideration
    for canonicalization.
  }];
}

def MaximumAndCover : Pass<"synth-maximum-and-cover"> {
  let summary = "Maximum And Cover for And-Inverter";
  let description = [{
    This pass performs maximum AND-cover optimization by collapsing single-fanout
    and-inverter ops into their users
  }];
}

def SOPBalancing : CutRewriterPassBase<"synth-sop-balancing", "hw::HWModuleOp"> {
  let summary = "SOP (Sum-of-Products) balancing for delay optimization";
  let description = [{
    This pass optimizes delay by restructuring the logic network using balanced
    sum-of-products expressions. It enumerates cuts, derives their SOP
    representation, and selects the best cut based on delay and area.

    The algorithm is based on "Delay Optimization Using SOP Balancing" by
    Mishchenko et al. (ICCAD 2011).
  }];
  let options =
      baseOptions#[Option<"maxCutInputSize", "max-cut-input-size", "unsigned",
                          /*default=*/"6", "Maximum number of inputs per cut">];
  let dependentDialects = ["synth::SynthDialect", "comb::CombDialect"];
}

def RegisterCorrespondence
    : Pass<"synth-register-correspondence", "hw::HWModuleOp"> {
  let summary = "Merge registers that are equivalent in all reachable states";
  let description = [{
    Register correspondence: register bits that are equal, complementary or
    constant in every reachable state are merged (one register per class;
    the others are replaced by it, its inverse or the constant). This is the
    register part of ABC's `scorr` (C. A. J. van Eijk, "Sequential
    Equivalence Checking Based on Structural Similarities", IEEE TCAD 19(7),
    2000).

    Candidate classes come from random simulation from the initial state.
    They are proved by induction: they hold in the initial state, and if all
    of them hold in a state, they hold in the next one (checked with a SAT
    solver); a counterexample to the inductive step splits the classes, and
    the check is repeated until no class changes. Only registers on the
    module's most common clock, without reset or inner symbol and with a
    constant (or no) initial value are candidates. Bits that are not and-gates
    of `synth.aig.and_inv`, wiring or constants, or are such registers, are
    free inputs of the check, which can only make it fail. If the check does
    not converge within `max-iterations`, nothing is merged. A register with
    a merged bit is split into one register per bit.

    With `internal`, i1 and-gates are candidates as well (signal
    correspondence, as ABC's `scorr`): a gate may be merged with a register,
    another gate, or a constant when they are equal in every reachable state
    for every input. The base case is then checked with SAT for all inputs of
    the initial state, and the inductive step over two cycles. A class's
    representative is a register if it has one, otherwise its first gate in
    topological order, so merging never creates a combinational cycle.

    The module keeps its outputs in every reachable state from the initial
    state; it is meant for models that are checked from that state (e.g. by
    a model checker after ExportAIGER).
  }];
  let options = [
    Option<"numPatterns", "num-patterns", "unsigned", "256",
           "Simulation patterns (a multiple of 64)">,
    Option<"numFrames", "num-frames", "unsigned", "32",
           "Simulated cycles from the initial state">,
    Option<"seed", "seed", "uint64_t", "0", "Seed of the simulation">,
    Option<"maxIterations", "max-iterations", "unsigned", "1000",
           "Induction rounds before giving up (then nothing is merged)">,
    Option<"conflictLimit", "conflict-limit", "int64_t", "10000",
           "Per-SAT-call conflict budget (-1: none); a register whose check "
           "exceeds it is not merged">,
    Option<"satSolver", "sat-solver", "std::string", "\"auto\"",
           "SAT solver backend: auto, z3 or cadical">,
    Option<"internal", "internal", "bool", "false",
           "Also merge i1 and-gates (signal correspondence)">,
    Option<"cexFrames", "cex-frames", "unsigned", "8",
           "Cycles simulated after a counterexample of the inductive step "
           "to split more classes">,
    Option<"chunk", "chunk", "unsigned", "256",
           "Pairs checked by one SAT query, with one incremental solver "
           "(0: one query per pair, speculatively reduced)">
  ];
  let statistics = [
    Statistic<"numCandidatesStat", "num-candidates",
              "Register bits in candidate classes after simulation">,
    Statistic<"numIterationsStat", "num-iterations", "Induction rounds">,
    Statistic<"numSATCallsStat", "num-sat-calls", "SAT calls">,
    Statistic<"numUnknownStat", "num-unknown",
              "SAT calls that exceeded the conflict budget">,
    Statistic<"numMergedStat", "num-merged", "Register bits merged">,
    Statistic<"numMergedGatesStat", "num-merged-gates",
              "And-gates merged (with internal)">
  ];
  let dependentDialects = ["comb::CombDialect", "hw::HWDialect",
                           "seq::SeqDialect", "synth::SynthDialect"];
}

def RemoveFalseCycles : Pass<"synth-remove-false-cycles", "hw::HWModuleOp"> {
  let summary = "Remove combinational cycles that are functionally acyclic";
  let description = [{
    Removes combinational cycles of `synth.aig.and_inv`, `comb.extract`,
    `comb.concat` and `comb.replicate` ops whose value is nevertheless well
    defined, so that passes that need a topological order can run.

    The ops of each cycle are rebuilt bit by bit; cycles that exist only at
    word level disappear by that alone. For a cycle of bits, a set of cut
    bits X is chosen so that the rest is acyclic, giving the cut bits as a
    function X' = F(X, inputs). If F^k (F applied k times) does not depend
    on X, which is checked with a SAT solver for k up to `max-unroll`, the
    cycle has the unique fixed point X = F^k(0, inputs), and the cut bits
    are replaced by k acyclic copies of the cycle logic computing it. This
    is an exact transformation. Cycles that fail the check are left alone
    with a warning.

    A cycle may only be sensitized in states the design never reaches, which
    a combinational check cannot see. With `obligation`, such cycles are
    rebuilt with `obligation-unroll` applications, and the module gets
    inputs Y1, Y2 per cycle and an output `false_cycle_obligation` that is 1
    when F^k(Y1) != F^k(Y2). The result is equivalent to the original module
    if this output is 0 in every reachable state of the result (by induction
    over the steps of a run: in such a state the cycle has the unique fixed
    point F^k(0)). This must be checked, e.g. by a model checker, before the
    result is used.

    With `fixpoint-obligation`, the cut bits are instead computed as the
    fixed point among all 2^n values v of the n cut bits (F(v) == v,
    enumerated, up to `max-fixpoint-cuts` bits), and the obligation output is
    1 unless exactly one v is a fixed point. This needs a unique fixed point
    rather than convergence of F, i.e. it takes the cycle to mean its unique
    consistent assignment (a cycle that is not constructive in ternary
    simulation, which a simulator may not settle, passes this check).
  }];
  let options = [
    Option<"maxUnroll", "max-unroll", "unsigned", "4",
           "Maximum number of applications of the cycle logic to try">,
    Option<"maxConeBits", "max-cone-bits", "unsigned", "200000",
           "Maximum number of bits of the fan-in logic encoded for the SAT "
           "check (bits beyond are free, which may only fail the check)">,
    Option<"satSolver", "sat-solver", "std::string", "\"auto\"",
           "SAT solver backend to use: auto, z3, or cadical">,
    Option<"conflictLimit", "conflict-limit", "int64_t", "100000",
           "Per-SAT-call conflict budget. -1 disables the limit.">,
    Option<"obligation", "obligation", "bool", "false",
           "Rebuild cycles that fail the check as well, with an obligation "
           "output that must be proved 0 in all reachable states">,
    Option<"obligationUnroll", "obligation-unroll", "unsigned", "1",
           "Number of applications of the cycle logic for obligations">,
    Option<"fixpointObligation", "fixpoint-obligation", "bool", "false",
           "With obligation: compute cut bits as the unique fixed point by "
           "enumeration instead of iteration">,
    Option<"maxFixpointCuts", "max-fixpoint-cuts", "unsigned", "8",
           "Maximum number of cut bits of a cycle for fixpoint-obligation">
  ];
  let statistics = [
    Statistic<"numObligations", "num-obligations",
              "Number of cycles rebuilt under an obligation">,
    Statistic<"numWordCycles", "num-word-cycles",
              "Number of cycles of ops rebuilt bit by bit">,
    Statistic<"numBitCycles", "num-bit-cycles",
              "Number of cycles of bits proved functionally acyclic">,
    Statistic<"numCuts", "num-cuts", "Number of cut bits">,
    Statistic<"numFailed", "num-failed",
              "Number of cycles of bits left (dependence or unknown)">
  ];
  let dependentDialects = ["comb::CombDialect", "hw::HWDialect"];
}

def FunctionalReduction : Pass<"synth-functional-reduction", "hw::HWModuleOp"> {
  let summary = "Functional reduction by simulation-guided candidate merging";
  let description = [{
    This pass performs functional reduction for single-bit `synth` logic. It
    runs random-pattern simulation to build candidate equivalence classes for
    `i1` values with matching signatures, then uses a SAT solver to verify
    each candidate pair before materializing the proven equivalences with
    `synth.choice` operations.

    See "FRAIGs: A Unifying Representation for Logic Synthesis and Verification,
    Tech. Rep., 2005" by Mishchenko et al. for details on the underlying
    algorithm.
  }];

  let options = [
    Option<"testTransformation", "test-transformation", "bool", "false",
           "Enable test transformation mode. In this mode, candidates are "
           "verified only when both values in a pair carry matching "
           "`synth.test.functional_reduction_equivalence_class` attributes.">,
    Option<"numRandomPatterns", "num-random-patterns", "unsigned", "1024",
           "Number of random simulation patterns (must be a positive multiple of 64; "
           "each pattern is one bit in a 64-bit word)">,
    Option<"seed", "seed", "unsigned", "0",
           "Seed for random pattern generation">,
    Option<"satSolver", "sat-solver", "std::string", "\"auto\"",
           "SAT solver backend to use: auto, z3, or cadical">,
    Option<"conflictLimit", "conflict-limit", "int64_t", "100",
           "Per-SAT-call conflict budget. -1 disables the limit.">,
    Option<"useChoices", "use-choices", "bool", "true",
           "Keep equivalent nodes as alternatives of synth.choice ops. "
           "Without choices, each node is replaced by its representative "
           "(skipped where that would create a combinational cycle).">
  ];
  let statistics = [
    Statistic<"numEquivClasses", "num-equiv-classes",
              "Number of non-trivial equivalence classes after simulation">,
    Statistic<"numProvedEquiv", "num-proved-equiv",
              "Number of equivalences proven by SAT">,
    Statistic<"numDisprovedEquiv", "num-disproved-equiv",
              "Number of equivalences disproved by SAT (CEX found)">,
    Statistic<"numUnknown", "num-unknown",
              "Number of SAT calls hitting resource limit (UNKNOWN)">,
    Statistic<"numMergedNodes", "num-merged-nodes",
              "Number of nodes merged due to functional equivalence">
  ];
  let dependentDialects = ["synth::SynthDialect"];
}

def ExactSynthesis : Pass<"synth-exact-synthesis"> {
  let summary = "Rewrite small comb.truth_table ops with exact synthesis";
  let description = [{
    This pass exact-synthesizes small single-bit `comb.truth_table` operations
    into Synth IR using a SAT-based minimum-area search. Unsupported or
    unsolved truth tables are left unchanged. The allowed primitive list
    controls the internal nodes selected by the SAT search. Direct constants,
    projections, and polarity inversions are handled as helper cases; an
    output inversion is materialized as `synth.aig.and_inv` and does not count
    against the area limit.
  }];
  let options = [
    ListOption<"allowedOps", "allowed-ops", "std::string",
               "Allowed BooleanLogicOpInterface primitives, written as "
               "'operation-name:arity' entries such as "
               "'synth.aig.and_inv:2'. May be specified multiple times.">,
    Option<"maxInputs", "max-inputs", "unsigned", "6",
           "Maximum number of truth-table inputs to synthesize">,
    Option<"maxArea", "max-area", "unsigned", "32",
           "Maximum number of internal nodes to search">,
    Option<"satSolver", "sat-solver", "std::string", "\"auto\"",
           "SAT solver backend to use: auto, z3, or cadical">
  ];
  let statistics = [
    Statistic<"numSolved", "solved", "Number of truth tables synthesized">,
    Statistic<"numUnsolved", "unsolved",
              "Number of truth tables left unchanged">
  ];
  let dependentDialects = ["hw::HWDialect", "synth::SynthDialect"];
}

def TernaryConstants : Pass<"synth-ternary-constants", "hw::HWModuleOp"> {
  let summary = "Replace registers that are constant in all reachable states";
  let description = [{
    Ternary simulation from the initial state: inputs, unknown initial values
    and bits the pass does not model are X (as in
    `synth-register-correspondence`), and each register's set of possible
    values is joined with its next-state value until nothing changes. The
    result over-approximates the reachable states, so a register bit that
    is still 0 or 1 is that constant in every reachable state and is
    replaced by it (a register with such a bit is split into one register
    per bit). No SAT solver is used; `synth-register-correspondence` proves
    more (equivalences, and constants that need induction), at a higher
    cost.
  }];
  let statistics = [
    Statistic<"numConstantStat", "num-constant",
              "Register bits replaced by a constant">
  ];
  let dependentDialects = ["comb::CombDialect", "hw::HWDialect",
                           "seq::SeqDialect", "synth::SynthDialect"];
}

def CheckConstraints : Pass<"synth-check-constraints", "hw::HWModuleOp"> {
  let summary = "Check that the constraints can hold";
  let description = [{
    Checks that the constraints of the module (`verif.assume` of an i1
    value, with an optional enable) can hold together from the initial
    state: for each cycle up to `depth`, some trace satisfies all of them in
    every cycle so far. If none does, the properties hold vacuously from that
    cycle on, which usually is a bug in the constraints (`:noConflict` of
    formal tools). The pass reports the cycle and a subset of the
    constraints that already conflict, as a warning (or an error with
    `fail-on-conflict`). It does not change the IR.

    Registers on the main clock with a constant (or no) initial value are
    modeled; other values are treated as free, which only adds traces, so a
    reported conflict is one of the module.
  }];
  let options = [
    Option<"depth", "depth", "unsigned", "20", "Number of cycles to check">,
    Option<"conflictLimit", "conflict-limit", "int", "100000",
           "Conflict limit per SAT call (-1: none)">,
    Option<"satSolver", "sat-solver", "std::string", "\"auto\"",
           "SAT solver backend">,
    Option<"failOnConflict", "fail-on-conflict", "bool", "false",
           "Report a conflict as an error">
  ];
  let statistics = [
    Statistic<"numConflictsStat", "num-conflicts", "Modules with a conflict">,
    Statistic<"numSATCallsStat", "num-sat-calls", "SAT calls">
  ];
}

def ForwardRetime : Pass<"synth-forward-retime", "hw::HWModuleOp"> {
  let summary = "Move registers forward through and-gates, cycle-exactly";
  let description = [{
    An i1 `synth.aig.and_inv` whose operands are all registers on one clock
    (no reset, constant or no initial value) that nothing else reads becomes
    one register of the gate over their next-state values, with the gate
    over their initial values as initial value (in ternary logic: unknown
    initial values give an unknown one only when they decide the gate).
    Every signal keeps its value in every cycle, so properties and
    counterexample depths do not change; the module has fewer registers.
    Repeated until no gate qualifies. Multi-bit registers are split into
    one register per bit first.
  }];
  let dependentDialects = ["comb::CombDialect", "seq::SeqDialect"];
  let statistics = [
    Statistic<"numMovedStat", "num-moved", "Gates moved past their registers">,
    Statistic<"numRemovedStat", "num-removed", "Registers removed (net)">
  ];
}

#endif // CIRCT_DIALECT_SYNTH_TRANSFORMS_PASSES_TD
