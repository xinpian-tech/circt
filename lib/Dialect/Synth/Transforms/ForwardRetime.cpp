//===- ForwardRetime.cpp - Move registers forward through and-gates -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward retiming that keeps every signal cycle-exact: an i1 and-gate whose
// operands are all registers on one clock, read by nothing else, becomes one
// register of the gate over their next-state values; its initial value is
// the gate over their initial values. The gate has the same value in every
// cycle, so properties and counterexamples (with their depth) are unchanged;
// the module has fewer registers (n become 1).
//
// Unknown initial values (no initial value) are handled with ternary logic:
// the operands are distinct registers, independent in the initial state, so
// the gate is 0 if an operand is 0, 1 if all are 1, and any value otherwise,
// which is a register without an initial value.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Synth/SynthOps.h"
#include "circt/Dialect/Synth/Transforms/SynthPasses.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "llvm/ADT/SetVector.h"

namespace circt {
namespace synth {
#define GEN_PASS_DEF_FORWARDRETIME
#include "circt/Dialect/Synth/Transforms/SynthPasses.h.inc"
} // namespace synth
} // namespace circt

using namespace circt;
using namespace mlir;
using namespace circt::synth;

namespace {
/// Ternary initial value: 0, 1, or unknown.
enum class Init { Zero, One, X };

/// A register that can be retimed: integer, no reset, no inner symbol, and a
/// constant initial value or none. Returns its initial value bits (empty
/// optional: unknown).
static bool getConstantInit(seq::CompRegOp op, std::optional<APInt> &init) {
  init.reset();
  auto initial = op.getInitialValue();
  if (!initial)
    return true;
  APInt c;
  if (!initial.getDefiningOp<seq::InitialOp>() ||
      !matchPattern(seq::unwrapImmutableValue(initial), m_ConstantInt(&c)))
    return false;
  init = c;
  return true;
}

static bool isRetimable(seq::CompRegOp op) {
  std::optional<APInt> init;
  return isa<IntegerType>(op.getType()) && !op.getReset() &&
         !op.getInnerSymAttr() && getConstantInit(op, init);
}

struct ForwardRetimePass
    : public circt::synth::impl::ForwardRetimeBase<ForwardRetimePass> {
  using ForwardRetimeBase::ForwardRetimeBase;

  void runOnOperation() override;

private:
  Value initialValue(OpBuilder &builder, Location loc, bool bit);
  /// Returns whether a register was split.
  bool splitRegisters();

  hw::HWModuleOp module;
  Value initials[2];
};
} // namespace

Value ForwardRetimePass::initialValue(OpBuilder &builder, Location loc,
                                      bool bit) {
  auto &v = initials[bit];
  if (!v) {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToStart(module.getBodyBlock());
    v = seq::createConstantInitialValue(
        builder, loc, builder.getIntegerAttr(builder.getI1Type(), bit));
  }
  return v;
}

/// Splits the retimable multi-bit registers into one register per bit, and
/// folds the wiring around them so that gates read the bits directly.
bool ForwardRetimePass::splitRegisters() {
  SmallVector<seq::CompRegOp> wide;
  for (auto op : module.getBodyBlock()->getOps<seq::CompRegOp>())
    if (isRetimable(op) && !op.getType().isInteger(1))
      wide.push_back(op);
  for (auto op : wide) {
    std::optional<APInt> init;
    (void)getConstantInit(op, init);
    OpBuilder builder(op);
    auto loc = op.getLoc();
    unsigned w = cast<IntegerType>(op.getType()).getWidth();
    SmallVector<Value> bits;
    for (unsigned i = 0; i < w; ++i) {
      Value next = comb::ExtractOp::create(builder, loc, op.getInput(), i, 1);
      Value initValue = init ? initialValue(builder, loc, (*init)[i]) : Value();
      StringAttr name;
      if (auto n = op.getNameAttr())
        name = builder.getStringAttr(n.getValue() + "[" + Twine(i) + "]");
      bits.push_back(seq::CompRegOp::create(builder, loc, next, op.getClk(),
                                            Value(), Value(), name,
                                            hw::InnerSymAttr(), initValue));
    }
    SmallVector<Value> msbFirst(llvm::reverse(bits));
    Value whole = comb::ConcatOp::create(builder, loc, msbFirst);
    op.getResult().replaceAllUsesWith(whole);
    op.erase();
  }
  if (wide.empty())
    return false;
  // Fold extract(concat) and the like, on the wiring ops only.
  RewritePatternSet patterns(&getContext());
  comb::ExtractOp::getCanonicalizationPatterns(patterns, &getContext());
  comb::ConcatOp::getCanonicalizationPatterns(patterns, &getContext());
  GreedyRewriteConfig config;
  config.setRegionSimplificationLevel(GreedySimplifyRegionLevel::Disabled);
  SmallVector<Operation *> wiring;
  module.walk([&](Operation *op) {
    if (isa<comb::ExtractOp, comb::ConcatOp>(op))
      wiring.push_back(op);
  });
  (void)applyOpPatternsGreedily(wiring, std::move(patterns), config);
  return true;
}

void ForwardRetimePass::runOnOperation() {
  module = getOperation();
  initials[0] = initials[1] = Value();
  bool split = splitRegisters();

  // The i1 register of `v`, if it can be moved.
  auto regOf = [](Value v) -> seq::CompRegOp {
    auto reg = v.getDefiningOp<seq::CompRegOp>();
    if (!reg || !reg.getType().isInteger(1) || !isRetimable(reg))
      return {};
    return reg;
  };

  SetVector<aig::AndInverterOp> worklist;
  for (auto op : module.getBodyBlock()->getOps<aig::AndInverterOp>())
    if (op.getType().isInteger(1))
      worklist.insert(op);
  unsigned moved = 0, removed = 0;
  while (!worklist.empty()) {
    auto gate = worklist.pop_back_val();
    if (gate.getInputs().size() < 2)
      continue;
    // All operands are movable registers on one clock, read only by the gate.
    SmallVector<seq::CompRegOp> regs;
    Value clock;
    bool ok = true;
    for (auto in : gate.getInputs()) {
      auto reg = regOf(in);
      if (!reg || (clock && reg.getClk() != clock) ||
          llvm::any_of(reg->getUsers(),
                       [&](Operation *u) { return u != gate; })) {
        ok = false;
        break;
      }
      clock = reg.getClk();
      regs.push_back(reg);
    }
    if (!ok)
      continue;
    // The initial value of the gate in ternary logic. The same register may
    // be an operand twice: with both phases the gate is 0.
    DenseMap<Operation *, bool> phase;
    bool contradiction = false;
    Init init = Init::One;
    for (auto [reg, inv] : llvm::zip(regs, gate.getInverted())) {
      auto [it, inserted] = phase.insert({reg, inv});
      if (!inserted && it->second != inv)
        contradiction = true;
      std::optional<APInt> bits;
      (void)getConstantInit(reg, bits);
      Init v = !bits ? Init::X : ((*bits)[0] != inv ? Init::One : Init::Zero);
      if (v == Init::Zero)
        init = Init::Zero;
      else if (v == Init::X && init == Init::One)
        init = Init::X;
    }
    if (contradiction)
      init = Init::Zero;

    OpBuilder builder(gate);
    auto loc = gate.getLoc();
    SmallVector<Value> nexts;
    for (auto reg : regs)
      nexts.push_back(reg.getInput());
    auto next =
        aig::AndInverterOp::create(builder, loc, nexts, gate.getInverted());
    Value initValue;
    if (init != Init::X)
      initValue = initialValue(builder, loc, init == Init::One);
    auto reg =
        seq::CompRegOp::create(builder, loc, next, clock, Value(), Value(),
                               StringAttr(), hw::InnerSymAttr(), initValue);
    gate.replaceAllUsesWith(reg.getResult());
    gate.erase();
    for (auto [op, _] : phase) {
      op->erase();
      ++removed;
    }
    ++moved;
    // The new register may let its readers move.
    for (auto *user : reg->getUsers())
      if (auto g = dyn_cast<aig::AndInverterOp>(user))
        if (g.getType().isInteger(1))
          worklist.insert(g);
  }
  numMovedStat += moved;
  numRemovedStat += removed - moved;
  if (!moved && !split)
    markAllAnalysesPreserved();
}
