//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the ContextSimplify pass: combinational logic
// simplified in the context where it matters, on words. A conjunct of an i1
// `and` only matters when the other conjuncts hold, a disjunct of an `or`
// when the others do not, an arm of a mux when its condition selects it.
// These facts are turned into known bits (a comparison `x == C` makes all
// bits of `x` known, a negated one an i1 known, a concatenation or extract
// passes them on), and the operand is evaluated on known bits under them
// (bounded depth). A conjunct known 1 in its context is dropped, one known 0
// makes the `and` 0 (dually for `or`), and an arm wholly known in its
// context becomes a constant. This catches the priority decoders of case
// statements, `x == A && !(x == B) && ...` with distinct constants, that
// rewriting does not and that bit-level SAT sweeping finds after
// bit-blasting.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Seq/SeqPasses.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/KnownBits.h"

#include "KnownBitsContext.h"

namespace circt {
namespace seq {
#define GEN_PASS_DEF_CONTEXTSIMPLIFY
#include "circt/Dialect/Seq/SeqPasses.h.inc"
} // namespace seq
} // namespace circt

using namespace mlir;
using namespace circt;
using llvm::KnownBits;
using namespace circt::seq::detail;

namespace {
struct ContextSimplifyPass
    : public seq::impl::ContextSimplifyBase<ContextSimplifyPass> {
  using ContextSimplifyBase::ContextSimplifyBase;
  void runOnOperation() override;
};

} // namespace

void ContextSimplifyPass::runOnOperation() {
  Block *body = getOperation().getBodyBlock();
  SmallVector<Operation *> ops;
  for (auto &op : *body)
    if (isa<comb::AndOp, comb::OrOp, comb::MuxOp>(op))
      ops.push_back(&op);
  OpBuilder builder(getOperation().getContext());
  unsigned dropped = 0, folded = 0, arms = 0, redundant = 0;
  for (auto *op : ops) {
    if (auto mux = dyn_cast<comb::MuxOp>(op)) {
      for (unsigned arm = 1; arm <= 2; ++arm) {
        Value a = op->getOperand(arm);
        if (constantOf(a))
          continue;
        Context ctx(budget, depth);
        ctx.addKnown(mux.getCond(), KnownBits::makeConstant(APInt(1, arm == 1)),
                     depth);
        if (ctx.contradiction)
          continue;
        KnownBits k = ctx.eval(a, depth);
        if (!k.isConstant())
          continue;
        builder.setInsertionPoint(op);
        op->setOperand(arm, hw::ConstantOp::create(builder, op->getLoc(),
                                                   k.getConstant()));
        ++arms;
      }
      // Bits where an arm equals the other one whenever the other one is
      // selected: there the mux is that arm. (A reset mux around logic that
      // already selects the reset value under reset.)
      unsigned w = widthOf(op->getResult(0));
      if (!w)
        continue;
      // An arm that is the other one whenever the other one is selected,
      // following muxes whose conditions are known there (a case chain that
      // already selects the value).
      bool whole = false;
      for (unsigned arm = 1; arm <= 2 && !whole; ++arm) {
        Context ctx(budget, depth);
        ctx.addKnown(mux.getCond(), KnownBits::makeConstant(APInt(1, arm == 2)),
                     depth);
        if (ctx.contradiction)
          continue;
        if (ctx.resolve(op->getOperand(arm)) ==
            ctx.resolve(op->getOperand(3 - arm))) {
          op->getResult(0).replaceAllUsesWith(op->getOperand(arm));
          ++redundant;
          whole = true;
        }
      }
      if (whole)
        continue;
      APInt same[3] = {APInt(w, 0), APInt(w, 0), APInt(w, 0)};
      for (unsigned arm = 1; arm <= 2; ++arm) {
        Context ctx(budget, depth);
        ctx.addKnown(mux.getCond(), KnownBits::makeConstant(APInt(1, arm == 2)),
                     depth);
        if (ctx.contradiction)
          continue;
        KnownBits mine = ctx.eval(op->getOperand(arm), depth);
        KnownBits other = ctx.eval(op->getOperand(3 - arm), depth);
        same[arm] = (mine.Zero & other.Zero) | (mine.One & other.One);
      }
      unsigned arm = same[1].popcount() >= same[2].popcount() ? 1 : 2;
      APInt mask = same[arm];
      if (mask.isZero())
        continue;
      Value a = op->getOperand(arm);
      if (mask.isAllOnes()) {
        op->getResult(0).replaceAllUsesWith(a);
        ++redundant;
        continue;
      }
      builder.setInsertionPointAfter(op);
      SmallVector<Value> parts;
      SmallPtrSet<Operation *, 8> created;
      for (int hi = w; hi > 0;) {
        bool isSame = mask[hi - 1];
        int lo = hi - 1;
        while (lo > 0 && mask[lo - 1] == isSame)
          --lo;
        auto e = comb::ExtractOp::create(
            builder, op->getLoc(), isSame ? a : op->getResult(0), lo, hi - lo);
        parts.push_back(e);
        if (!isSame)
          created.insert(e);
        hi = lo;
      }
      op->getResult(0).replaceAllUsesExcept(
          comb::ConcatOp::create(builder, op->getLoc(), parts), created);
      ++redundant;
      continue;
    }
    if (widthOf(op->getResult(0)) != 1 || op->getNumOperands() < 2)
      continue;
    bool isAnd = isa<comb::AndOp>(op);
    // The value that decides (0 for and, 1 for or) and the neutral one.
    bool absorbing = !isAnd;
    SmallVector<Value> operands(op->getOperands());
    bool decided = false;
    for (unsigned i = 0; i < operands.size() && !decided;) {
      Context ctx(budget, depth);
      for (unsigned j = 0; j < operands.size(); ++j)
        if (j != i)
          ctx.addKnown(operands[j], KnownBits::makeConstant(APInt(1, isAnd)),
                       depth);
      if (ctx.contradiction) {
        // The others cannot all be neutral: the result is absorbing.
        decided = true;
        break;
      }
      KnownBits k = ctx.eval(operands[i], depth);
      if (k.isConstant()) {
        if (k.getConstant().getBoolValue() == isAnd) {
          operands.erase(operands.begin() + i);
          ++dropped;
          continue;
        }
        decided = true;
        break;
      }
      ++i;
    }
    if (decided) {
      builder.setInsertionPoint(op);
      op->getResult(0).replaceAllUsesWith(
          hw::ConstantOp::create(builder, op->getLoc(), APInt(1, absorbing)));
      ++folded;
      continue;
    }
    if (operands.size() == op->getNumOperands())
      continue;
    builder.setInsertionPoint(op);
    Value result;
    if (operands.empty())
      result = hw::ConstantOp::create(builder, op->getLoc(), APInt(1, isAnd));
    else if (operands.size() == 1)
      result = operands[0];
    else
      result = isAnd
                   ? comb::AndOp::create(builder, op->getLoc(), operands, false)
                         .getResult()
                   : comb::OrOp::create(builder, op->getLoc(), operands, false)
                         .getResult();
    op->getResult(0).replaceAllUsesWith(result);
  }
  numDropped += dropped;
  numFolded += folded;
  numArms += arms;
  numRedundant += redundant;
  if (!dropped && !folded && !arms && !redundant)
    markAllAnalysesPreserved();
}
