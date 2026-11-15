//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the AbstractOperators pass: multipliers become
// uninterpreted functions. The result of each multiplier is a new input of
// the module, constrained by assumptions that hold for a multiplication:
//
//  - consistency: two multipliers with equal operands (in either order) have
//    equal results, in the same cycle and against the operands and result of
//    every multiplier in the previous cycle, which registers keep;
//  - range: operands zero-extended from ka and kb bits give a result of at
//    most (2^ka - 1) * (2^kb - 1); sign-extended, a result of magnitude at
//    most 2^(ka + kb - 2).
//
// With multipliers of different widths the low bits are compared. The result
// over-approximates the module: a proof carries over, a counterexample may be
// spurious.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Verif/VerifOps.h"
#include "circt/Dialect/Verif/VerifPasses.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include <set>

using namespace mlir;
using namespace circt;

namespace circt {
namespace verif {
#define GEN_PASS_DEF_ABSTRACTOPERATORSPASS
#include "circt/Dialect/Verif/Passes.h.inc"
} // namespace verif
} // namespace circt

namespace {
struct AbstractOperatorsPass
    : public verif::impl::AbstractOperatorsPassBase<AbstractOperatorsPass> {
  using AbstractOperatorsPassBase::AbstractOperatorsPassBase;
  void runOnOperation() override;
};

/// An abstracted multiplier, or the record of one in the previous cycle.
struct Application {
  Value lhs, rhs, result;
  unsigned width;
};
} // namespace

/// The widths of the values an operand is built from (through muxes,
/// concatenations, extracts and replications): the widths it may be an
/// extension from.
static void collectWidths(Value value, unsigned limit, std::set<unsigned> &out,
                          DenseSet<Value> &visited, unsigned depth = 0) {
  if (!visited.insert(value).second || depth > 16)
    return;
  unsigned width = hw::getBitWidth(value.getType());
  // Zero-extended from k bits is also sign-extended from k + 1.
  for (unsigned candidate : {width, width + 1})
    if (candidate >= 2 && candidate < limit)
      out.insert(candidate);
  auto *def = value.getDefiningOp();
  if (!def || !isa<comb::MuxOp, comb::ConcatOp, comb::ExtractOp,
                   comb::ReplicateOp>(def))
    return;
  for (auto operand : def->getOperands())
    collectWidths(operand, limit, out, visited, depth + 1);
}

void AbstractOperatorsPass::runOnOperation() {
  auto module = getOperation();
  auto *body = module.getBodyBlock();

  SmallVector<comb::MulOp> multipliers;
  Value clock;
  for (auto &op : *body) {
    if (auto mul = dyn_cast<comb::MulOp>(&op)) {
      if (mul.getNumOperands() != 2 ||
          hw::getBitWidth(mul.getType()) < int64_t(minWidth))
        continue;
      if (llvm::any_of(mul.getOperands(), [](Value operand) {
            return operand.getDefiningOp<hw::ConstantOp>();
          }))
        continue;
      multipliers.push_back(mul);
    } else if (auto reg = dyn_cast<seq::CompRegOp>(&op)) {
      if (!clock)
        clock = reg.getClk();
    }
  }
  if (multipliers.empty() || multipliers.size() > maxOperators)
    return markAllAnalysesPreserved();

  OpBuilder builder(body->getTerminator());
  unsigned numAssumed = 0;

  auto lowBits = [&](Location loc, Value value, unsigned width) -> Value {
    if (hw::getBitWidth(value.getType()) == width)
      return value;
    return comb::ExtractOp::create(builder, loc, value, 0, width);
  };
  auto assume = [&](Location loc, Value premise, Value conclusion) {
    Value holds = comb::OrOp::create(
        builder, loc, comb::createOrFoldNot(builder, loc, premise, true),
        conclusion, true);
    verif::AssumeOp::create(builder, loc, holds, Value(), StringAttr());
    ++numAssumed;
  };
  // Equal low operand bits give equal low result bits.
  auto consistent = [&](Location loc, const Application &a,
                        const Application &b) {
    unsigned width = std::min(a.width, b.width);
    auto equal = [&](Value x, Value y) -> Value {
      return comb::ICmpOp::create(builder, loc, comb::ICmpPredicate::eq,
                                  lowBits(loc, x, width),
                                  lowBits(loc, y, width), true);
    };
    Value results = equal(a.result, b.result);
    assume(loc,
           comb::AndOp::create(builder, loc, equal(a.lhs, b.lhs),
                               equal(a.rhs, b.rhs), true),
           results);
    assume(loc,
           comb::AndOp::create(builder, loc, equal(a.lhs, b.rhs),
                               equal(a.rhs, b.lhs), true),
           results);
  };
  // The bits of `value` from `from` up are all zero / all equal.
  auto highZero = [&](Location loc, Value value, unsigned from) -> Value {
    unsigned width = hw::getBitWidth(value.getType());
    Value high = comb::ExtractOp::create(builder, loc, value, from, width - from);
    Value zero = hw::ConstantOp::create(builder, loc, APInt(width - from, 0));
    return comb::ICmpOp::create(builder, loc, comb::ICmpPredicate::eq, high,
                                zero, true);
  };
  auto highEqual = [&](Location loc, Value value, unsigned from) -> Value {
    // Sign-extended from `from` bits: bits from - 1 up are equal.
    unsigned width = hw::getBitWidth(value.getType());
    unsigned count = width - from + 1;
    Value high = comb::ExtractOp::create(builder, loc, value, from - 1, count);
    Value zero = hw::ConstantOp::create(builder, loc, APInt(count, 0));
    Value ones = hw::ConstantOp::create(builder, loc, APInt::getAllOnes(count));
    return comb::OrOp::create(
        builder, loc,
        comb::ICmpOp::create(builder, loc, comb::ICmpPredicate::eq, high, zero,
                             true),
        comb::ICmpOp::create(builder, loc, comb::ICmpPredicate::eq, high, ones,
                             true),
        true);
  };

  // Replace the multipliers by inputs.
  SmallVector<Application> applications;
  SmallVector<Location> locs;
  for (auto [index, mul] : llvm::enumerate(multipliers)) {
    unsigned width = hw::getBitWidth(mul.getType());
    auto input = module.appendInput("abstract_mul_" + Twine(index),
                                    mul.getType());
    applications.push_back(
        {mul.getOperand(0), mul.getOperand(1), input.second, width});
    locs.push_back(mul.getLoc());
    mul.getResult().replaceAllUsesWith(input.second);
    mul.erase();
  }
  // appendInput may have rebuilt the block; the builder still points before
  // the terminator.
  builder.setInsertionPoint(module.getBodyBlock()->getTerminator());

  // The previous cycle.
  SmallVector<Application> previous;
  if (history && clock)
    for (auto [index, application] : llvm::enumerate(applications)) {
      auto reg = [&](Value value, const Twine &name) -> Value {
        return seq::CompRegOp::create(
            builder, locs[index], value, clock, Value(), Value(),
            builder.getStringAttr("abstract_mul_" + Twine(index) + name),
            hw::InnerSymAttr(), Value());
      };
      previous.push_back({reg(application.lhs, "_lhs"),
                          reg(application.rhs, "_rhs"),
                          reg(application.result, "_result"),
                          application.width});
    }

  for (unsigned i = 0, e = applications.size(); i != e; ++i) {
    auto loc = locs[i];
    auto &application = applications[i];
    for (unsigned j = i + 1; j != e; ++j)
      consistent(loc, application, applications[j]);
    for (auto &record : previous)
      consistent(loc, application, record);

    // Range.
    std::set<unsigned> lhsWidths, rhsWidths;
    DenseSet<Value> visited;
    collectWidths(application.lhs, application.width, lhsWidths, visited);
    visited.clear();
    collectWidths(application.rhs, application.width, rhsWidths, visited);
    for (unsigned lhsWidth : lhsWidths)
      for (unsigned rhsWidth : rhsWidths) {
        unsigned resultWidth = lhsWidth + rhsWidth;
        if (resultWidth >= application.width)
          continue;
        // Unsigned: the product is at most (2^ka - 1) * (2^kb - 1).
        APInt bound = (APInt::getLowBitsSet(application.width, lhsWidth) *
                       APInt::getLowBitsSet(application.width, rhsWidth));
        assume(loc,
               comb::AndOp::create(builder, loc,
                                   highZero(loc, application.lhs, lhsWidth),
                                   highZero(loc, application.rhs, rhsWidth),
                                   true),
               comb::ICmpOp::create(
                   builder, loc, comb::ICmpPredicate::ule, application.result,
                   hw::ConstantOp::create(builder, loc, bound), true));
        // Signed: the product is sign-extended from ka + kb bits and its
        // magnitude is at most 2^(ka + kb - 2).
        APInt magnitude =
            APInt::getOneBitSet(application.width, resultWidth - 2);
        Value inRange = comb::AndOp::create(
            builder, loc,
            comb::ICmpOp::create(
                builder, loc, comb::ICmpPredicate::sle, application.result,
                hw::ConstantOp::create(builder, loc, magnitude), true),
            comb::ICmpOp::create(
                builder, loc, comb::ICmpPredicate::sge, application.result,
                hw::ConstantOp::create(builder, loc, -magnitude), true),
            true);
        assume(loc,
               comb::AndOp::create(builder, loc,
                                   highEqual(loc, application.lhs, lhsWidth),
                                   highEqual(loc, application.rhs, rhsWidth),
                                   true),
               inRange);
      }
  }
  // Outputs to replay a counterexample: a trace is one of the original
  // module if every result is the product of its operands in every cycle.
  if (observe)
    for (auto [index, application] : llvm::enumerate(applications)) {
      auto name = "abstract_mul_" + Twine(index);
      module.appendOutput((name + "_obs_lhs").str(), application.lhs);
      module.appendOutput((name + "_obs_rhs").str(), application.rhs);
      module.appendOutput((name + "_obs_result").str(), application.result);
    }
  numAbstracted += applications.size();
  numAssumptions += numAssumed;
}
