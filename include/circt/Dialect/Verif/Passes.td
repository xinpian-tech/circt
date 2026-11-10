//===-- Passes.td - Verif pass definition file -------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the passes that work on the Verif dialect.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_VERIF_PASSES_TD
#define CIRCT_DIALECT_VERIF_PASSES_TD

include "mlir/Pass/PassBase.td"

def CombineAssertLikePass : Pass<"combine-assert-like"> {
  let summary = "Combines all assertions and assumptions into single operations";
  let description = [{
    Combines all assertions and assumptions by conjoining their conditions into a
    single assertion and assumption respectively, e.g.
    ```mlir
    // [...]
    verif.assume %pred0 : i1
    verif.assume %pred1 : i1
    // [...]
    verif.assert %cond0 : i1
    verif.assert %cond1 : i1
    ```
    will be converted into  
    ```
    // [...]
    %pred = comb.and bin %pred0, %pred1 : i1
    verif.assume %pred : i1
    // [...]
    %cond = comb.and bin %cond0, %cond1 : i1
    verif.assert %cond : i1
    ```
    This is already implicitly done in many of down stream tools, and being 
    able to simplify assertions early can help will other verification tasks 
    such as contract inference.

    The conjunctions are created on a per-block basis, so asserlike ops in 
    separate blocks will not be merged.
  }];
}

def FoldAssumePass : Pass<"fold-assume"> { 
  let summary = "Folds all assumes into the enable signal of an assert.";
  let description = [{
    Implements assumes manually by combining all of them into a knowledge caucus, that
    is then used as the antecedant of every assertion within the same region. This 
    assumes that `combine-assert-like` is run prior to running this pass, as it will 
    fail if there is more than one assertion and one assumption per block. 
    E.g., 

    ```mlir
    // [...]
    verif.assume %conjoinedPred : i1
    // [...]
    verif.assert %cond : i1
    ```
    is converted to
    ```mlir  
    // [...]
    // [...]
    verif.assert %cond if %conjoinedPred : i1
    ```

    This is a semantically preserving transformation in the formal back-ends as 
    assumes have the following underlying semantics:

                            {a0, ..., ak} ∈ A
      -------------------------------------------------------------------------
      Γ; A ⊢ assume {a0, ... ak}; assert a -> assert ((a0 and .. and ak) => a)
    
    Therefore, assertions become trivial when assumptions do no hold for a concrete value, 
    making them equivalent to an enable signal.
  }];
}

def PrepareForFormalPass : Pass<"prepare-for-formal", "hw::HWModuleOp"> {
  let summary = "Prepares a given top-level circuit for formal verification";
  let description = [{
    Prepares a circuit for formal verification. This performs semantic-retaining 
    transformations that make model checking easier, e.g. flattening wires,
    handling formal contracts, removing unwanted information such as OM Classes,
    etc.
  }];
}

def VerifyClockedAssertLikePass : Pass<"verify-clocked-assert-like",
                                       "hw::HWModuleOp"> {
  let summary = "Check that clocked-assert-like are valid";
  let description = [{
    Checks that every `clocked_assert`, `clocked_assume` or `clocked_cover` op
    does not contain a nested `ltl.clock` or `ltl.disable` operation in its
    operand tree.
  }];
}

def LowerTestsPass : Pass<"verif-lower-tests", "mlir::ModuleOp"> {
  let summary = "Convert Verif tests into HW modules";
  let description = [{
    Converts `verif.formal` and `verif.simulation` operations into top-level
    `hw.module` operations that can be fed to formal verification and simulation
    tools.
  }];
}

def LowerContractsPass : Pass<"lower-contracts", "mlir::ModuleOp"> {
  let summary = "Lower contracts into formal tests";
  let description = [{
    For `hw.module` ops containing any `verif.contract` ops, apply the
    contracts in the `hw.module` body and emit a `verif.formal` op that checks
    each applied contract.

    Contracts are checked by turning `require`s into `assume`s and `ensure`s
    into `asserts`s.  Inputs to the `hw.module` op are replaced with
    `verif.symbolic_value` ops and other contracts are assumed to hold by
    inlining the contract body.

    Contracts are applied by turning `require`s into `assert`s and `ensure`s
    into `assume`s in the original `hw.module` op body.  Results of the
    contract are replaced with `verif.symbolic_value` ops.  In some cases,
    using the `SimplifyAssumeEqPass` after lowering contracts and before
    canonicalization and cse can result in a greatly simplified `hw.module` op
    body.
  }];
}

def LowerSymbolicValuesPass : Pass<"verif-lower-symbolic-values",
                                   "mlir::ModuleOp"> {
  let summary = "Lower symbolic values to blackbox instances, wires, or inputs";
  let description = [{
    Converts `verif.symbolic_value` ops into one of the following
    representations that formal tools can work with:

    - Instances of an external module, which most formal tools can treat as an
      internal port boundary with symbolic values assigned to the module
      outputs.
    - Wire declarations marked with `(* anyseq *)`, which Yosys treats like a
      top-level input port.
    - Input ports on the containing `hw.module`, for downstream lowerings that
      already model module inputs as unconstrained values.
  }];
  let options = [
    Option<
      "mode", "mode",
      "SymbolicValueLowering", "SymbolicValueLowering::ExtModule",
      "Control how symbolic values are lowered",
      "symbolicValueLoweringCLValues()"
    >,
  ];
  let dependentDialects = [
    "hw::HWDialect",
    "sv::SVDialect",
  ];
}

def SimplifyAssumeEqPass : Pass<"simplify-assume-eq", "hw::HWModuleOp"> {
  let summary = "Use assume equal statements to simplify symbolic values";
  let description = [{
    When a symbolic value is assumed equal to another value, the symbolic value
    is replaced with its equal value to simplify.
  }];
}

def SplitAssumesPass : Pass<"verif-split-assumes", "hw::HWModuleOp"> {
  let summary = "Split assumed conjunctions into one assumption per operand";
  let description = [{
    A `verif.assume` of an i1 conjunction (`comb.and`, or a
    `synth.aig.and_inv` of several operands) without enable becomes one
    assumption per operand (an inverted operand of `and_inv` gets an
    inverter); an assumption of the constant true is removed. Later passes
    then see the individual facts (e.g. `verif-substitute-assumed-inputs`).
  }];
  let statistics = [
    Statistic<"numSplit", "num-split", "Assumptions split">
  ];
  let dependentDialects = ["synth::SynthDialect"];
}

def SubstituteAssumedInputsPass
    : Pass<"verif-substitute-assumed-inputs", "hw::HWModuleOp"> {
  let summary = "Replace input bits that the assumptions determine";
  let description = [{
    Uses the assumptions of the module (`verif.assume` of an i1 without
    enable, which hold in every cycle) to eliminate inputs: an input bit x
    (an i1 input or a 1-bit extract of an input) that an assumption makes
    constant (`assume x`, `assume !x`) or equal to another value h (both
    `!(x & !h)` and `!(!x & h)` assumed, as `verif-split-assumes` leaves an
    assumed XNOR, or `x == h` / `x != h`), with h not depending on x
    combinationally, is replaced by its value everywhere except in the
    assumptions that determine it.

    Those assumptions keep reading the input, so the result has the same
    traces as the module: the input still has the value the assumptions
    give it, and a counterexample of the result is one of the module without
    mapping. Unlike `simplify-assume-eq`, which replaces symbolic values,
    this handles inputs that take a new value in every cycle.
  }];
  let statistics = [
    Statistic<"numConstant", "num-constant", "Input bits made constant">,
    Statistic<"numEqual", "num-equal", "Input bits replaced by a value">
  ];
  let dependentDialects = ["hw::HWDialect", "synth::SynthDialect"];
}

def AssertionsToOutputPass : Pass<"verif-assertions-to-output",
                                  "mlir::ModuleOp"> {
  let summary = "Turn clocked assertions and assumptions into a safety output";
  let description = [{
    Replaces the clocked assertions and assumptions of each module by one i1
    output that is 1 in a cycle where an assertion is violated and all
    assumptions held up to and including that cycle (AIGER `bad` semantics).

    A property is an i1, a sequence of fixed delays (`ltl.delay` with length
    0, `ltl.concat`), or an implication between such sequences, the forms
    left by lower-ltl-to-core for SVA such as `a ##1 b |-> ##2 c`. Such an
    implication is checked in the cycle where its consequent ends, with
    registers (starting at 0) for the earlier steps, and its enable must
    hold in every step. All assertions must use the rising edge of the same
    clock.

    `asserts` selects assertions by label (default: all); a selected
    assertion that is missing or unsupported is an error, other unsupported
    properties are left out with a warning. `assumes` lists labels of
    assertions to use as assumptions. With `replace-outputs` (default), the
    output replaces the module's outputs. With `constraints`, each
    assumption becomes a `verif.assume` of the i1 that must hold in every
    cycle, and the output is 1 where an assertion is violated: the same
    semantics, with the assumptions kept apart (as AIGER constraints) so
    that later passes can use them.
  }];
  let options = [
    ListOption<"asserts", "asserts", "std::string",
               "Labels of the assertions to check (default: all)">,
    ListOption<"assumes", "assumes", "std::string",
               "Labels of assertions to use as assumptions">,
    Option<"outputName", "output-name", "std::string", "\"bad\"",
           "Name of the output">,
    Option<"replaceOutputs", "replace-outputs", "bool", "true",
           "Replace the outputs of the module by the new output">,
    Option<"constraints", "constraints", "bool", "false",
           "Keep the assumptions as `verif.assume` ops of the condition that "
           "must hold in every cycle (AIGER constraints) instead of folding "
           "them into the output">
  ];
  let statistics = [
    Statistic<"numAsserts", "num-asserts", "Number of assertions checked">,
    Statistic<"numAssumes", "num-assumes", "Number of assumptions">,
    Statistic<"numSkipped", "num-skipped",
              "Number of unsupported properties left out">
  ];
  let dependentDialects = ["comb::CombDialect", "hw::HWDialect",
                           "seq::SeqDialect"];
}

def SimplifyODCPass : Pass<"verif-simplify-odc", "hw::HWModuleOp"> {
  let summary = "Simplify assertions with observability don't-cares";
  let description = [{
    A value in the logic of a property only has to be right where the
    property observes it: the property of an assertion where its enable
    holds, the consequent of `a |-> c` where `a` holds, the operands of
    `mux(s, x, y)` where `s` is 1 or 0, the second operand of an and (or)
    where the first one is 1 (0). This pass reads facts from these conditions
    (conjuncts, negated disjuncts, and `x == C` comparisons), accumulates
    them along the paths from the property, and substitutes them into a copy
    of the logic. The design logic is not modified. With `antecedent-only`,
    only the enable and the antecedent are used, which substitutes the
    antecedent into the consequent.

    The copy is canonicalized and merged with structurally equal logic of the
    cone (ignoring discardable attributes such as `sv.namehint`). The facts
    typically decide the muxes that select operands, so that both sides of a
    datapath check become the same value and the consequent folds to true; a
    remark is emitted for such assertions. If it does not fold, the pass
    tries case splits on 1-bit signals that select muxes: when the consequent
    folds to true for `s == v`, the assertion becomes
    `(a && s != v) |-> c[s := !v]`.

    Applies to `verif.assert`, `verif.assume` and their clocked forms whose
    property is a boolean or an `ltl.implication` of booleans. Run
    canonicalize afterwards to remove the logic left unused.
  }];
  let options = [
    Option<"antecedentOnly", "antecedent-only", "bool", "false",
           "Only use the enable and the antecedent, not the don't-cares "
           "inside the consequent">,
    Option<"maxClones", "max-clones", "unsigned", "10000",
           "Stop opening new contexts after this many copied ops per "
           "assertion">,
    Option<"maxSplits", "max-splits", "unsigned", "4",
           "Maximum number of case splits per assertion">
  ];
  let statistics = [
    Statistic<"numRewritten", "num-rewritten",
              "Number of assertions whose logic was rewritten">,
    Statistic<"numClones", "num-clones", "Number of ops copied">,
    Statistic<"numSplits", "num-splits", "Number of case splits">,
    Statistic<"numProved", "num-proved",
              "Number of assertions whose consequent folded to true">
  ];
}

def StripContractsPass : Pass<"strip-contracts"> {
  let summary = "Remove contracts from the IR";
  let description = [{
    Replaces all `verif.contract` ops in the IR with their operands, making them
    act as simple passthroughs.
  }];
}

def ShiftTargetPass : Pass<"verif-shift-target", "hw::HWModuleOp"> {
  let summary = "Check the assertions one cycle ahead (target shift)";
  let description = [{
    Each `verif.assert` `p` (i1) becomes `(first -> p) && (c' -> p')`: `p'`
    is `p` in the next cycle, built from the current one (registers are
    replaced by their next-state input, inputs by new inputs
    `NAME_shiftK` for their value in the next cycle), `c'` the
    `verif.assume`s in the next cycle, and `first` a new register that is 1
    in the initial cycle only. A counterexample of depth t maps to one of
    depth t + 1 of the original (or t = 0 from `first`), with the new inputs
    as the inputs of the last cycle; a bounded proof to depth k covers
    k + 1. Repeated `steps` times; the module records the total in
    `verif.target_shift`. Registers with reset, several clocks and
    non-combinational ops in the cones are rejected.
  }];
  let options = [
    Option<"steps", "steps", "unsigned", "1", "Number of cycles to shift">
  ];
  let dependentDialects = ["seq::SeqDialect", "synth::SynthDialect"];
}

#endif // CIRCT_DIALECT_VERIF_PASSES_TD
