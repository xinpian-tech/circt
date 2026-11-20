//===-- Passes.td - Comb pass definition file --------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_COMB_PASSES_TD
#define CIRCT_DIALECT_COMB_PASSES_TD

include "mlir/Pass/PassBase.td"

def LowerComb : Pass<"lower-comb"> {
  let summary = "Lowers the some of the comb ops";
  let description = [{
    Some operations in the comb dialect (e.g. `comb.truth_table`) are not
    directly supported by ExportVerilog. They need to be lowered into ops which
    are supported. There are many ways to lower these ops so we do this in a
    separate pass. This also allows the lowered form to participate in
    optimizations like the comb canonicalizers.
  }];
}

def CombIntRangeNarrowing : Pass<"comb-int-range-narrowing"> {
  let summary = "Reduce comb op bitwidth based on integer range analysis.";
  let description = [{
    Compute a basic value range analysis, by propagating integer intervals 
    through the domain. The analysis is limited by a lack of sign-extension 
    operator in the comb dialect, leading to an over-approximation. 
    Particularly for signed arithmetic, a single interval is often an 
    over-approximation, a more precise analysis would require a union of 
    intervals.
  }];
}

def CombOverflowAnnotating : Pass<"comb-overflow-annotating"> {
  let summary = "Annotate comb ops with no overflow signal (nuw).";
  let description = [{
    Using integer range analysis, annotate comb ops which cannot overflow with
    LLVMs no unsigned wrap (nuw) signal. This is useful for downstream passes 
    which need to handle overflow conditions.
  }];
}

def BalanceMux : Pass<"comb-balance-mux"> {
  let summary = "Balance and optimize mux chains";
  let description = [{
    Optimizes mux chains through enhanced folding and priority mux
    rebalancing. Converts index comparisons to arrays and rebalances
    linear chains into balanced trees, reducing depth from O(n) to O(log n).
  }];
  let options = [Option<
      "muxChainThreshold", "mux-chain-threshold", "unsigned", "16",
      "Minimum number of linear mux chains to trigger rebalancing">];
}

def AssumeTwoValued : Pass<"comb-assume-two-valued"> {
  let summary = "Simplify under assumption of two-valued logic";
  let description = [{
    Performs simplifying transformations that are valid if all integer values
    are always two-valued (i.e., all bits are either 1 or 0).
  }];
}


def SimplifyTruthTable : Pass<"comb-simplify-tt"> {
  let summary = "Simplify truth tables that depend on 1 or less inputs";
  let description = [{
    Simplifies truth tables that are constant or depend only on single input.
    For truth tables that depend on no input, reduce them to hw.constant. For
    truth tables that depend on single input, reduce to identity of input or
    negation of input.
  }];
}

def MuxDontCare : Pass<"comb-mux-dont-care", "hw::HWModuleOp"> {
  let summary = "Zero the bits of mux arms that no shift can bring out";
  let description = [{
    For a mux whose result is only shifted or extracted, finds the bits of
    each arm that can reach a used bit of the result while the arm is
    selected, and replaces the others by zero:

      r = shru(mux(c, a, b), k)[31:0], c implies k >= 8
      ->  a's bits 0..7 are never seen; a := {a[63:8], 8'b0}

    The values of the select and of the shift amounts are enumerated over
    the bits they depend on (registers, ports and other operators are free
    inputs), so the rewrite is exact and needs no reachability. Bits that
    are only seen through such a mux, like the low byte of a register that
    holds the first half of a misaligned load, then have no observer left.
  }];
  let dependentDialects = ["comb::CombDialect", "hw::HWDialect"];
  let options = [
    Option<"maxSupportBits", "max-support-bits", "unsigned", "12",
           "Maximum number of free bits the select and shift amounts may "
           "depend on together">
  ];
  let statistics = [
    Statistic<"numArms", "num-arms", "Number of mux arms rewritten">,
    Statistic<"numBits", "num-bits", "Number of arm bits replaced by zero">,
  ];
}

def SplitCycles : Pass<"comb-split-cycles", "hw::HWModuleOp"> {
  let summary = "Split word-level combinational cycles that bits do not have";
  let description = [{
    Aggregates lowered to integers can give combinational cycles of words
    that their bits do not have (a field of a concatenation feeding another
    field of it). Such a cycle of bitwise operations, muxes, concatenations,
    extracts and replications is split into bits; values used outside the
    cycle are rebuilt as concatenations of their bits. A cycle through any
    other operation, or one that is a cycle of bits too, is left alone.
    Word-level back ends (BTOR2, SMT) need the module free of cycles.
  }];
  let statistics = [
    Statistic<"numSplitOps", "num-split-ops", "Operations split into bits">,
    Statistic<"numKeptCycles", "num-kept-cycles", "Cycles left alone">,
  ];
  let dependentDialects = ["hw::HWDialect"];
}

#endif // CIRCT_DIALECT_COMB_PASSES_TD
