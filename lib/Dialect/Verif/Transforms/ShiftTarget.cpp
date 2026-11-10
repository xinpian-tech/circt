//===- ShiftTarget.cpp - Check assertions one cycle ahead -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Target shift (target enlargement by one step): each assertion `p` becomes
//
//   (first -> p) && (c' -> p')
//
// where `p'` is the value of `p` in the next cycle, built from the current
// one (a register is its next-state input, an input is a new input that
// stands for its value in the next cycle), `c'` is the constraints
// (`verif.assume`) in the next cycle, and `first` is a new register that is 1
// in the initial cycle only. A failure of the new assertion in cycle t is a
// failure of `p` in cycle t (from `first`, t = 0) or in cycle t + 1 (with
// the new inputs as the inputs of cycle t + 1); constraints still hold in
// every cycle up to it. So counterexamples get one cycle shorter: a solver
// that finds one at depth t maps it back to depth t + 1, and a bounded proof
// to depth k covers depth k + 1. The module records the total shift in the
// `verif.target_shift` attribute.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombDialect.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Synth/SynthDialect.h"
#include "circt/Dialect/Synth/SynthOps.h"
#include "circt/Dialect/Verif/VerifOps.h"
#include "circt/Dialect/Verif/VerifPasses.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/IRMapping.h"

namespace circt {
namespace verif {
#define GEN_PASS_DEF_SHIFTTARGETPASS
#include "circt/Dialect/Verif/Passes.h.inc"
} // namespace verif
} // namespace circt

using namespace circt;
using namespace mlir;

namespace {
struct ShiftTargetPass
    : public verif::impl::ShiftTargetPassBase<ShiftTargetPass> {
  using ShiftTargetPassBase::ShiftTargetPassBase;
  void runOnOperation() override;

private:
  /// One shift of the module; fails (with an error) on what it cannot shift.
  LogicalResult shiftOnce();
  /// The value of `root` in the next cycle.
  FailureOr<Value> next(Value root);

  hw::HWModuleOp module;
  OpBuilder *builder = nullptr;
  DenseMap<Value, Value> nextValue;
  /// The number of shifts done so far, for the names of new inputs.
  unsigned round = 0;
};
} // namespace

/// Ops that are pure combinational logic and can be copied.
static bool isCombinational(Operation *op) {
  if (op->getNumRegions() != 0 || op->getNumResults() == 0)
    return false;
  if (isa<hw::ConstantOp, hw::BitcastOp, hw::ArrayCreateOp, hw::ArrayGetOp,
          hw::ArraySliceOp, hw::ArrayConcatOp, hw::StructCreateOp,
          hw::StructExtractOp, hw::AggregateConstantOp>(op))
    return true;
  auto *dialect = op->getDialect();
  return isa<comb::CombDialect, synth::SynthDialect>(dialect);
}

FailureOr<Value> ShiftTargetPass::next(Value root) {
  SmallVector<std::pair<Value, bool>> stack{{root, false}};
  while (!stack.empty()) {
    auto [v, expanded] = stack.pop_back_val();
    if (nextValue.count(v))
      continue;
    if (auto arg = dyn_cast<BlockArgument>(v)) {
      if (isa<seq::ClockType>(arg.getType())) {
        nextValue[v] = v;
        continue;
      }
      auto name = module.getInputName(arg.getArgNumber());
      auto [_, newArg] = module.appendInput(
          Twine(name) + "_shift" + Twine(round), arg.getType());
      nextValue[v] = newArg;
      continue;
    }
    Operation *op = v.getDefiningOp();
    if (auto reg = dyn_cast<seq::CompRegOp>(op)) {
      if (reg.getReset())
        return reg.emitError("cannot shift through a register with reset");
      nextValue[v] = reg.getInput();
      continue;
    }
    if (op->hasTrait<OpTrait::ConstantLike>()) {
      nextValue[v] = v;
      continue;
    }
    if (!isCombinational(op))
      return op->emitError("cannot shift through this op");
    if (!expanded) {
      stack.push_back({v, true});
      for (auto operand : op->getOperands())
        stack.push_back({operand, false});
      continue;
    }
    IRMapping mapping;
    for (auto operand : op->getOperands())
      mapping.map(operand, nextValue.at(operand));
    auto *copy = builder->clone(*op, mapping);
    for (auto [old, nw] : llvm::zip(op->getResults(), copy->getResults()))
      nextValue[old] = nw;
  }
  return nextValue.at(root);
}

LogicalResult ShiftTargetPass::shiftOnce() {
  auto *body = module.getBodyBlock();
  SmallVector<verif::AssertOp> asserts;
  SmallVector<verif::AssumeOp> assumes;
  Value clock;
  for (auto &op : *body) {
    if (auto a = dyn_cast<verif::AssertOp>(op)) {
      if (!a.getProperty().getType().isInteger(1))
        return a.emitError("cannot shift a non-i1 assertion");
      asserts.push_back(a);
    } else if (auto a = dyn_cast<verif::AssumeOp>(op)) {
      if (!a.getProperty().getType().isInteger(1))
        return a.emitError("cannot shift a non-i1 assumption");
      assumes.push_back(a);
    } else if (auto reg = dyn_cast<seq::CompRegOp>(op)) {
      if (clock && reg.getClk() != clock)
        return reg.emitError("cannot shift a module with several clocks");
      clock = reg.getClk();
    } else if (isa<verif::CoverOp, verif::ClockedAssertOp,
                   verif::ClockedAssumeOp>(op)) {
      return op.emitError("cannot shift this verification op");
    }
  }
  if (asserts.empty())
    return success();
  if (!clock) {
    // A combinational module: the next cycle is the same check.
    return success();
  }

  OpBuilder b(body->getTerminator());
  builder = &b;
  nextValue.clear();
  auto loc = module.getLoc();
  // `!en | p` of an assertion-like op.
  auto holds = [&](Value prop, Value en) -> Value {
    if (!en)
      return prop;
    auto bad = synth::aig::AndInverterOp::create(b, loc, en, prop, false, true);
    return synth::aig::AndInverterOp::create(b, loc, bad, true);
  };

  // The constraints in the next cycle.
  Value constraintsNext;
  SmallVector<Value> cs;
  for (auto a : assumes) {
    auto n = next(holds(a.getProperty(), a.getEnable()));
    if (failed(n))
      return failure();
    cs.push_back(*n);
  }
  if (cs.size() == 1)
    constraintsNext = cs[0];
  else if (!cs.empty())
    constraintsNext = synth::aig::AndInverterOp::create(
        b, loc, cs, SmallVector<bool>(cs.size(), false));

  // first: 1 in the initial cycle only.
  Value initTrue = seq::createConstantInitialValue(
      b, loc, b.getIntegerAttr(b.getI1Type(), 1));
  Value zero = hw::ConstantOp::create(b, loc, b.getI1Type(), 0);
  Value first = seq::CompRegOp::create(b, loc, zero, clock, Value(), Value(),
                                       b.getStringAttr("shift_first"),
                                       hw::InnerSymAttr(), initTrue);

  for (auto a : asserts) {
    Value p = holds(a.getProperty(), a.getEnable());
    auto pNext = next(p);
    if (failed(pNext))
      return failure();
    // fail0 = first & !p; fail1 = c' & !p'.
    Value fail0 =
        synth::aig::AndInverterOp::create(b, loc, first, p, false, true);
    Value fail1 =
        constraintsNext
            ? Value(synth::aig::AndInverterOp::create(b, loc, constraintsNext,
                                                      *pNext, false, true))
            : Value(synth::aig::AndInverterOp::create(b, loc, *pNext, true));
    Value ok =
        synth::aig::AndInverterOp::create(b, loc, fail0, fail1, true, true);
    a.getPropertyMutable().assign(ok);
    a.getEnableMutable().clear();
  }
  return success();
}

void ShiftTargetPass::runOnOperation() {
  module = getOperation();
  // Nothing to shift without assertions: the module (and its shift) stays.
  auto asserts = module.getBodyBlock()->getOps<verif::AssertOp>();
  if (!steps || asserts.begin() == asserts.end())
    return markAllAnalysesPreserved();
  round = 0;
  if (auto old = module->getAttrOfType<IntegerAttr>("verif.target_shift"))
    round = old.getInt();
  for (unsigned i = 0; i < steps; ++i, ++round)
    if (failed(shiftOnce()))
      return signalPassFailure();
  module->setAttr("verif.target_shift",
                  IntegerAttr::get(IntegerType::get(&getContext(), 32), round));
}
