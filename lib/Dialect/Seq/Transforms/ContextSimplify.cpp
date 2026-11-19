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
// A negated comparison `x != C` is a disequality fact, passed on through
// bijections with constants (`x + C`, `C - x`, `x ^ C`); a disjunction among
// the facts (an `or` known 1, an `and` known 0) is split into its cases. In
// an arm, muxes whose condition is known where the arm is selected (also by
// refuting one value of the condition, through the operands of an `or` it
// would make 1) are their arm, and operands of an `or` known 0 there are
// dropped. The bits of a mux where one arm equals the other whenever the
// other is selected (by known bits, or slice by slice following muxes and
// splitting on conditions) are that arm. With `or-form`, a mux whose arms are
// disjoint (bits of one arm 0 whenever the other is selected) becomes the or
// of its arms, gated by the condition only where needed. SAT sweeping finds
// these (a mux redundant under a case split, one-hot muxes) after
// bit-blasting as and-gates equal to one of their inputs.
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

/// The bits of `a` where `mask` is set, of `b` elsewhere.
static Value selectBits(OpBuilder &builder, Location loc, const APInt &mask,
                        Value a, Value b) {
  if (mask.isAllOnes())
    return a;
  if (mask.isZero())
    return b;
  unsigned w = mask.getBitWidth();
  SmallVector<Value> parts;
  for (int hi = w; hi > 0;) {
    bool isA = mask[hi - 1];
    int lo = hi - 1;
    while (lo > 0 && mask[lo - 1] == isA)
      --lo;
    parts.push_back(
        comb::ExtractOp::create(builder, loc, isA ? a : b, lo, hi - lo));
    hi = lo;
  }
  return comb::ConcatOp::create(builder, loc, parts);
}

/// A mux `c ? a : b` where bits of `b` are 0 whenever `a` is selected (or
/// bits of `a` whenever `b` is) as the disjunction of its arms there, each
/// gated by the condition only where needed: `(c ? a : 0) | b` (an and less
/// per bit after bit-blasting). `zeroA` / `zeroB` are the bits of `a` known 0
/// when `c` is 0 / of `b` known 0 when `c` is 1.
static bool rewriteDisjoint(comb::MuxOp mux, APInt zeroA, APInt zeroB,
                            unsigned budget, unsigned depth) {
  Value a = mux.getTrueValue(), b = mux.getFalseValue();
  // Bits of an arm that are constant are gated by an and (or nothing)
  // anyway; changing them would only break up the mux.
  for (auto [arm, zero] : {std::make_pair(a, &zeroA), {b, &zeroB}}) {
    if (zero->isZero())
      continue;
    Context ctx(budget, depth);
    KnownBits k = ctx.eval(arm, depth);
    *zero &= ~(k.Zero | k.One);
  }
  if (zeroA.isZero() && zeroB.isZero())
    return false;
  unsigned w = zeroA.getBitWidth();
  OpBuilder builder(mux);
  builder.setInsertionPointAfter(mux);
  Location loc = mux.getLoc();
  Value zero = hw::ConstantOp::create(builder, loc, APInt(w, 0));
  Value gatedA = zeroA.isAllOnes()
                     ? a
                     : comb::MuxOp::create(builder, loc, mux.getCond(), a, zero)
                           .getResult();
  Value gatedB = zeroB.isAllOnes()
                     ? b
                     : comb::MuxOp::create(builder, loc, mux.getCond(), zero, b)
                           .getResult();
  Value left = selectBits(builder, loc, zeroA, a, gatedA);
  Value right = selectBits(builder, loc, zeroB, b, gatedB);
  mux.getResult().replaceAllUsesWith(
      comb::OrOp::create(builder, loc, left, right, false).getResult());
  return true;
}

/// `v` without the operands of an or known 0 under the facts (of an and
/// known all ones), in each case of splits on the disjunctions among them
/// (see Context::forEachCase), if it is used once.
static Value pruneUnder(Context &ctx, Value v, unsigned levels,
                        unsigned cases) {
  Operation *op = v.getDefiningOp();
  if (!op || !isa<comb::OrOp, comb::AndOp>(op) || op->getNumOperands() < 2 ||
      !v.hasOneUse())
    return v;
  bool isAnd = isa<comb::AndOp>(op);
  unsigned n = op->getNumOperands();
  SmallVector<bool> drop(n, true);
  bool possible = false;
  unsigned extra = cases ? cases - 1 : 0;
  ctx.forEachCase(levels, extra, [&](Context &c) {
    possible = true;
    for (unsigned i = 0; i < n; ++i) {
      if (!drop[i])
        continue;
      KnownBits k = c.eval(c.resolve(op->getOperand(i), 2), c.depth);
      drop[i] = isAnd ? k.One.isAllOnes() : k.Zero.isAllOnes();
    }
    return llvm::is_contained(drop, true);
  });
  if (!possible)
    return v;
  SmallVector<Value> kept;
  for (unsigned i = 0; i < n; ++i)
    if (!drop[i])
      kept.push_back(op->getOperand(i));
  if (kept.size() == op->getNumOperands())
    return v;
  OpBuilder builder(op);
  unsigned w = widthOf(v);
  if (kept.empty())
    return hw::ConstantOp::create(builder, op->getLoc(),
                                  isAnd ? APInt::getAllOnes(w) : APInt(w, 0));
  if (kept.size() == 1)
    return kept[0];
  if (isAnd)
    return comb::AndOp::create(builder, op->getLoc(), kept, false);
  return comb::OrOp::create(builder, op->getLoc(), kept, false);
}

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
      // Each arm under the facts where it is selected: muxes in it whose
      // condition is known there are their arm, operands of an or known 0
      // there (of an and known 1) are dropped, an arm wholly known there is
      // that constant. Then the bits where the other arm equals it there (by
      // known bits in each case of splits on the disjunctions among the
      // facts, or by slices; see equalMask): there the mux is the other arm.
      // (A reset mux around logic that already selects the reset value under
      // reset; a case chain that already selects the value.)
      unsigned w = widthOf(op->getResult(0));
      if (!w)
        continue;
      APInt same[3] = {APInt(w, 0), APInt(w, 0), APInt(w, 0)};
      // Bits of an arm known 0 when the other one is selected.
      APInt zero[3] = {APInt(w, 0), APInt(w, 0), APInt(w, 0)};
      // First each arm on its own, under the facts where it is selected.
      for (unsigned arm = 1; arm <= 2; ++arm) {
        Value v = op->getOperand(arm);
        if (constantOf(v))
          continue;
        Context ctx(budget, depth);
        if (!ctx.assume(mux.getCond(), arm == 1))
          continue;
        if (Value r = ctx.resolve(v, 2); r != v) {
          op->setOperand(arm, r);
          v = r;
          ++arms;
        }
        if (Value r = pruneUnder(ctx, v, splits, cases); r != v) {
          op->setOperand(arm, r);
          v = r;
          ++arms;
        }
        KnownBits k = ctx.eval(v, depth);
        if (k.isConstant() && !constantOf(v)) {
          builder.setInsertionPoint(op);
          op->setOperand(arm, hw::ConstantOp::create(builder, op->getLoc(),
                                                     k.getConstant()));
          ++arms;
        }
      }
      // Then each arm where the other one is selected.
      bool never = false;
      for (unsigned arm = 1; arm <= 2 && !never; ++arm) {
        unsigned other = 3 - arm;
        Context ctx(budget, depth);
        if (!ctx.assume(mux.getCond(), other == 1)) {
          // The other arm is never selected.
          same[arm].setAllBits();
          never = true;
          break;
        }
        Value mine = op->getOperand(arm), o = op->getOperand(other);
        APInt s = APInt::getAllOnes(w);
        std::optional<KnownBits> mineK;
        unsigned count = cases ? cases - 1 : 0;
        ctx.forEachCase(splits, count, [&](Context &c) {
          KnownBits m = c.eval(c.resolve(mine, 2), depth);
          KnownBits t = c.eval(c.resolve(o, 2), depth);
          s &= (m.Zero & t.Zero) | (m.One & t.One);
          mineK = mineK ? mineK->intersectWith(m) : m;
          return true;
        });
        if (!mineK) {
          // No case is possible.
          same[arm].setAllBits();
          never = true;
          break;
        }
        same[arm] = s;
        zero[arm] = mineK->Zero;
        if (same[arm].isAllOnes())
          break;
        unsigned count2 = steps;
        ctx.steps = &count2;
        same[arm] |=
            equalMask(ctx, mine, o, splits, [](Value, Value) { return false; });
        if (same[arm].isAllOnes())
          break;
      }
      unsigned arm = same[1].popcount() >= same[2].popcount() ? 1 : 2;
      APInt mask = same[arm];
      if (mask.isZero()) {
        if (orForm)
          redundant +=
              rewriteDisjoint(mux, zero[1], zero[2], budget, depth) ? 1 : 0;
        continue;
      }
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
