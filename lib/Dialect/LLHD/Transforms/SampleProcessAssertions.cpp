//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the SampleProcessAssertions pass. Immediate assertions
// in a clocked process (`assert` statements in `always @(posedge clk)`) are
// checked when the process runs at the rising edge, on the values the process
// sees then: the values of the cycle that ends. The pass copies the process
// into an `llhd.combinational` that runs its body once as at a rising edge
// (the past clock 0, the present clock 1) and keeps only what the assertions
// need, and removes the assertions from the process. The copy checks them in
// every cycle on the values of that cycle, which in a design on that clock is
// the same; `llhd-remove-control-flow` then turns the branches leading to them
// into enables. The process is left with no side effects for `llhd-deseq`.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "circt/Dialect/LLHD/LLHDPasses.h"
#include "circt/Dialect/Verif/VerifOps.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/SmallPtrSet.h"

namespace circt {
namespace llhd {
#define GEN_PASS_DEF_SAMPLEPROCESSASSERTIONSPASS
#include "circt/Dialect/LLHD/LLHDPasses.h.inc"
} // namespace llhd
} // namespace circt

using namespace mlir;
using namespace circt;
using namespace circt::llhd;

namespace {
struct SampleProcessAssertionsPass
    : public llhd::impl::SampleProcessAssertionsPassBase<
          SampleProcessAssertionsPass> {
  void runOnOperation() override;
};
} // namespace

static bool isImmediateAssertLike(Operation *op) {
  return isa<verif::AssertOp, verif::AssumeOp, verif::CoverOp>(op) &&
         op->getOperand(0).getType().isInteger(1);
}

/// Whether `value` is `!past & clock` (in either operand order, the inversion
/// as `xor` with true).
static bool isRisingEdge(Value value, Value past, Value clock) {
  auto andOp = value.getDefiningOp<comb::AndOp>();
  if (!andOp || andOp.getNumOperands() != 2)
    return false;
  auto isNotPast = [&](Value v) {
    auto xorOp = v.getDefiningOp<comb::XorOp>();
    return xorOp && xorOp.isBinaryNot() && xorOp.getOperand(0) == past;
  };
  Value a = andOp.getOperand(0), b = andOp.getOperand(1);
  return (isNotPast(a) && b == clock) || (isNotPast(b) && a == clock);
}

/// Copies the immediate assertions of `process` into a combinational region
/// that runs the process body as at a rising edge of its clock, and removes
/// them from the process. Returns false if the process does not have the
/// expected shape: one wait observing one clock, resuming in a block that
/// branches on its rising edge, and no side effects besides the assertions.
static bool sample(ProcessOp process) {
  SmallVector<Operation *> asserts;
  WaitOp wait;
  for (auto &block : process.getBody()) {
    for (auto &op : block) {
      if (isImmediateAssertLike(&op)) {
        asserts.push_back(&op);
        continue;
      }
      if (auto w = dyn_cast<WaitOp>(op)) {
        if (wait)
          return false;
        wait = w;
        continue;
      }
      if (isa<HaltOp, YieldOp>(op))
        continue;
      if (!isMemoryEffectFree(&op))
        return false;
    }
  }
  if (asserts.empty() || !wait || wait.getDelay() ||
      wait.getObserved().size() != 1)
    return false;
  Value clock = wait.getObserved()[0];
  if (!clock.getType().isInteger(1))
    return false;

  // The block after the wait gets the past clock and branches to the body on
  // its rising edge.
  Block *resume = wait.getDest();
  if (wait.getDestOperands().size() != 1 ||
      wait.getDestOperands()[0] != clock || resume->getNumArguments() != 1)
    return false;
  Value past = resume->getArgument(0);
  auto edgeBranch = dyn_cast<cf::CondBranchOp>(resume->getTerminator());
  if (!edgeBranch || !isRisingEdge(edgeBranch.getCondition(), past, clock) ||
      edgeBranch.getFalseDest() != wait->getBlock())
    return false;

  // Copy the process into a combinational region. The entry block keeps its
  // operations (they may be used throughout) and branches to the resumption
  // block with the past clock 0; the wait ends the evaluation.
  OpBuilder builder(process);
  auto combOp = CombinationalOp::create(builder, process.getLoc(), TypeRange{});
  IRMapping mapping;
  process.getBody().cloneInto(&combOp.getBody(), mapping);
  Block *entry = &combOp.getBody().front();
  auto *oldTerminator = entry->getTerminator();
  builder.setInsertionPoint(oldTerminator);
  Value low = hw::ConstantOp::create(builder, process.getLoc(), APInt(1, 0));
  Value high = hw::ConstantOp::create(builder, process.getLoc(), APInt(1, 1));
  cf::BranchOp::create(builder, process.getLoc(), mapping.lookup(resume),
                       ValueRange{low});
  oldTerminator->erase();
  for (auto &block : combOp.getBody()) {
    auto *terminator = block.getTerminator();
    if (!isa<WaitOp, HaltOp>(terminator))
      continue;
    builder.setInsertionPoint(terminator);
    YieldOp::create(builder, terminator->getLoc(), ValueRange{});
    terminator->erase();
  }
  clock.replaceUsesWithIf(high, [&](OpOperand &use) {
    return combOp->isProperAncestor(use.getOwner());
  });

  // Blocks that only the wait reached are now unreachable: drop them, after
  // removing their uses.
  SmallPtrSet<Block *, 16> reached;
  SmallVector<Block *> stack{entry};
  while (!stack.empty()) {
    Block *block = stack.pop_back_val();
    if (!reached.insert(block).second)
      continue;
    for (Block *succ : block->getSuccessors())
      stack.push_back(succ);
  }
  SmallVector<Block *> unreachable;
  for (auto &block : combOp.getBody())
    if (!reached.contains(&block))
      unreachable.push_back(&block);
  for (auto *block : unreachable)
    block->dropAllDefinedValueUses();
  for (auto *block : unreachable)
    block->erase();

  // The copy keeps the assertions and pure operations; the process keeps the
  // rest. Pure operations the assertions do not need are removed later.
  for (auto *op : asserts)
    op->erase();
  return true;
}

void SampleProcessAssertionsPass::runOnOperation() {
  for (auto process :
       llvm::make_early_inc_range(getOperation().getOps<ProcessOp>()))
    if (sample(process))
      ++numProcessesSampled;
}
