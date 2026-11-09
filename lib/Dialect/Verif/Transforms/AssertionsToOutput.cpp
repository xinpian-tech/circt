//===- AssertionsToOutput.cpp - Assertions to a safety output -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Turn the clocked assertions and assumptions of a module into one output
// that is 1 when an assertion is violated while all assumptions held so far,
// the form taken by safety model checkers (AIGER `bad`).
//
// A clocked assertion `P if EN` is violated in a cycle where EN & !P. P may
// also be an implication between sequences of fixed delays (`a ##1 b |->
// ##2 c`): it is checked when the consequent ends, with registers holding the
// earlier steps; EN must hold in every step, as for `disable iff`.
//
// All assertions are on one clock, or on gated versions of it
// (`seq.clock_gate`): those are checked in the cycles where the gate is
// enabled, and their registers count cycles of the gated clock.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LTL/LTLOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Verif/VerifOps.h"
#include "circt/Dialect/Verif/VerifPasses.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringSet.h"
#include <numeric>

using namespace circt;
using namespace mlir;

namespace circt {
namespace verif {
#define GEN_PASS_DEF_ASSERTIONSTOOUTPUTPASS
#include "circt/Dialect/Verif/Passes.h.inc"
} // namespace verif
} // namespace circt

namespace {
struct AssertionsToOutputPass
    : verif::impl::AssertionsToOutputPassBase<AssertionsToOutputPass> {
  using AssertionsToOutputPassBase::AssertionsToOutputPassBase;
  void runOnOperation() override;
  LogicalResult lower(hw::HWModuleOp module, llvm::StringSet<> &found);
};

/// A clocked assertion or assumption.
struct Clocked {
  Operation *op;
  Value property, enable, clock;
  verif::ClockEdge edge;
  StringRef label;
  bool isAssume;
  /// For an assertion on a gated clock: `enable` held in every cycle of the
  /// ungated clock since the previous cycle of the gated clock (a `disable
  /// iff` also applies between the cycles of the gated clock).
  Value enableSince;

  static std::optional<Clocked> get(Operation *op) {
    auto from = [&](auto clocked, bool isAssume) {
      return Clocked{op,
                     clocked.getProperty(),
                     clocked.getEnable(),
                     clocked.getClock(),
                     clocked.getEdge(),
                     clocked.getLabel() ? *clocked.getLabel() : "",
                     isAssume};
    };
    if (auto a = dyn_cast<verif::ClockedAssertOp>(op))
      return from(a, false);
    if (auto a = dyn_cast<verif::ClockedAssumeOp>(op))
      return from(a, true);
    return std::nullopt;
  }
};

/// A boolean of a sequence and the cycle in which it is checked, counted
/// from the start of the sequence.
struct Step {
  Value value;
  unsigned time;
};

class Lowering {
public:
  Lowering(OpBuilder &builder, Value clock) : builder(builder), clock(clock) {}

  /// The violation of `op` as an i1 value, or failure with a reason.
  FailureOr<Value> violation(const Clocked &op, std::string &reason);

  /// `value` delayed by `n` cycles (0 before the start).
  Value past(Value value, unsigned n);
  Value notOf(Value value) {
    return comb::createOrFoldNot(builder, value.getLoc(), value, true);
  }
  Value andOf(ArrayRef<Value> values, Location loc) {
    if (values.empty())
      return hw::ConstantOp::create(builder, loc, builder.getI1Type(), 1);
    if (values.size() == 1)
      return values[0];
    return comb::AndOp::create(builder, loc, values, true);
  }
  Value orOf(ArrayRef<Value> values, Location loc) {
    if (values.empty())
      return hw::ConstantOp::create(builder, loc, builder.getI1Type(), 0);
    if (values.size() == 1)
      return values[0];
    return comb::OrOp::create(builder, loc, values, true);
  }
  /// A 1-bit register starting at 0.
  seq::CompRegOp reg(Value input, Location loc) {
    if (!zero)
      zero = seq::createConstantInitialValue(
          builder, loc, builder.getIntegerAttr(builder.getI1Type(), 0));
    return seq::CompRegOp::create(builder, loc, input, clock, Value(), Value(),
                                  zero);
  }

  /// LTL ops that may be left without users.
  SmallVector<Operation *> visited;

private:
  /// The steps of a sequence of fixed delays; returns its length.
  FailureOr<unsigned> sequence(Value value, unsigned base,
                               SmallVectorImpl<Step> &steps,
                               std::string &reason);
  Value unwrap(Value value);

  OpBuilder &builder;
  Value clock;
  Value zero;
  DenseMap<std::pair<Value, unsigned>, Value> delayed;
};
} // namespace

Value Lowering::past(Value value, unsigned n) {
  if (n == 0)
    return value;
  auto &result = delayed[{value, n}];
  if (!result)
    result = reg(past(value, n - 1), value.getLoc());
  return result;
}

Value Lowering::unwrap(Value value) {
  while (auto *op = value.getDefiningOp()) {
    if (auto clockOp = dyn_cast<ltl::ClockOp>(op)) {
      visited.push_back(op);
      value = clockOp.getInput();
      continue;
    }
    if (auto cast = dyn_cast<UnrealizedConversionCastOp>(op);
        cast && cast.getNumOperands() == 1) {
      visited.push_back(op);
      value = cast.getOperand(0);
      continue;
    }
    break;
  }
  return value;
}

FailureOr<unsigned> Lowering::sequence(Value value, unsigned base,
                                       SmallVectorImpl<Step> &steps,
                                       std::string &reason) {
  value = unwrap(value);
  if (value.getType().isInteger(1)) {
    steps.push_back({value, base});
    return 0u;
  }
  auto *op = value.getDefiningOp();
  if (op)
    visited.push_back(op);
  if (auto delay = dyn_cast_or_null<ltl::DelayOp>(op)) {
    if (!delay.getLength() || *delay.getLength() != 0) {
      reason = "delay with a range";
      return failure();
    }
    unsigned d = delay.getDelay();
    auto n = sequence(delay.getInput(), base + d, steps, reason);
    if (failed(n))
      return failure();
    return *n + d;
  }
  if (auto concat = dyn_cast_or_null<ltl::ConcatOp>(op)) {
    unsigned end = 0;
    for (auto input : concat.getInputs()) {
      auto n = sequence(input, base + end, steps, reason);
      if (failed(n))
        return failure();
      end += *n;
    }
    return end;
  }
  reason = op ? ("sequence op " + op->getName().getStringRef()).str()
              : "sequence argument";
  return failure();
}

FailureOr<Value> Lowering::violation(const Clocked &op, std::string &reason) {
  auto loc = op.op->getLoc();
  Value enable = op.enable;
  Value property = unwrap(op.property);
  if (property.getType().isInteger(1)) {
    SmallVector<Value> terms{notOf(property)};
    if (enable)
      terms.push_back(enable);
    return andOf(terms, loc);
  }
  // A sequence S holds where it matches: `1 |-> S`.
  Value antecedent, consequent = property;
  if (auto impl = property.getDefiningOp<ltl::ImplicationOp>()) {
    visited.push_back(impl);
    antecedent = impl.getAntecedent();
    consequent = impl.getConsequent();
  } else if (!isa<ltl::SequenceType>(property.getType())) {
    auto *def = property.getDefiningOp();
    reason = def ? ("property op " + def->getName().getStringRef()).str()
                 : "property argument";
    return failure();
  }
  // The antecedent's steps, then the consequent's, which starts in the last
  // cycle of the antecedent. Checked when the consequent ends: a failing
  // step is reported up to `cend` cycles late, which is the same for safety.
  SmallVector<Step> steps, csteps;
  FailureOr<unsigned> end = 0u;
  if (antecedent)
    end = sequence(antecedent, 0, steps, reason);
  if (failed(end))
    return failure();
  auto cend = sequence(consequent, 0, csteps, reason);
  if (failed(cend))
    return failure();
  unsigned total = *end + *cend;
  SmallVector<Value> terms;
  for (auto step : steps)
    terms.push_back(past(step.value, total - step.time));
  SmallVector<Value> fails;
  for (auto step : csteps)
    fails.push_back(past(notOf(step.value), *cend - step.time));
  terms.push_back(orOf(fails, loc));
  // EN in the first step, and since then (for a gated clock: also in the
  // cycles in between).
  if (enable)
    for (unsigned k = 0; k <= total; ++k)
      terms.push_back(
          past(k == total || !op.enableSince ? enable : op.enableSince, k));
  return andOf(terms, loc);
}

LogicalResult AssertionsToOutputPass::lower(hw::HWModuleOp module,
                                            llvm::StringSet<> &found) {
  auto *body = module.getBodyBlock();
  SmallVector<Clocked> ops;
  for (auto &op : *body) {
    if (auto clocked = Clocked::get(&op)) {
      ops.push_back(*clocked);
      continue;
    }
    if (isa<verif::AssertOp, verif::AssumeOp>(op)) {
      op.emitError("unclocked assertion not supported by "
                   "verif-assertions-to-output");
      return failure();
    }
  }
  if (ops.empty())
    return success();

  // One clock, possibly gated: the clock of the output. An assertion on a
  // gated version of it (`seq.clock_gate`) is checked in the cycles where the
  // gate is enabled, and its registers are clocked by the gated clock (see
  // seq-clock-gate-to-enable).
  OpBuilder builder(body->getTerminator());
  auto loc = module.getLoc();
  auto baseOf = [&](Value clockI1, Value &enable) -> Value {
    Value value = clockI1;
    while (true) {
      auto fromClock = value.getDefiningOp<seq::FromClockOp>();
      if (!fromClock)
        return value;
      Value clock = fromClock.getInput();
      if (auto gate = clock.getDefiningOp<seq::ClockGateOp>()) {
        Value gateEnable = gate.getEnable();
        if (auto testEnable = gate.getTestEnable())
          gateEnable = comb::OrOp::create(builder, loc, gateEnable, testEnable,
                                          true);
        enable = enable ? comb::AndOp::create(builder, loc, enable, gateEnable,
                                              true)
                              .getResult()
                        : gateEnable;
        clock = gate.getInput();
      }
      if (auto toClock = clock.getDefiningOp<seq::ToClockOp>()) {
        value = toClock.getInput();
        continue;
      }
      return clock;
    }
  };
  SmallVector<Value> gateEnables;
  Value base;
  for (auto &op : ops) {
    Value enable;
    Value opBase = baseOf(op.clock, enable);
    gateEnables.push_back(enable);
    if (!base)
      base = opBase;
    if (opBase != base || op.edge != verif::ClockEdge::Pos) {
      op.op->emitError("verif-assertions-to-output needs all assertions on the "
                       "rising edge of the same clock (or gated versions "
                       "of it)");
      return failure();
    }
  }

  llvm::StringSet<> selected, assumed;
  for (auto &label : asserts)
    selected.insert(label);
  for (auto &label : assumes)
    assumed.insert(label);

  Value clock = isa<seq::ClockType>(base.getType())
                    ? base
                    : seq::ToClockOp::create(builder, loc, base).getResult();
  Lowering lowering(builder, clock);
  // One lowering per gated clock, for the registers of its assertions.
  DenseMap<Value, std::unique_ptr<Lowering>> gatedLowerings;
  SmallVector<Value> violations, assumptions;
  for (auto [op, gateEnable] : llvm::zip(ops, gateEnables)) {
    StringRef label = op.label;
    if (!label.empty())
      found.insert(label);
    bool isAssume = op.isAssume || assumed.contains(label);
    if (!isAssume && !selected.empty() && !selected.contains(label))
      continue;
    std::string reason;
    Lowering *opLowering = &lowering;
    Clocked gatedOp = op;
    if (gateEnable && op.enable) {
      // `enable` held in every ungated cycle since the last gated one:
      // `broken` is set by a cycle without it, and cleared by a gated cycle.
      auto opLoc = op.op->getLoc();
      auto broken = lowering.reg(lowering.orOf({}, opLoc), opLoc);
      Value brokenNow =
          lowering.orOf({broken, lowering.notOf(op.enable)}, opLoc);
      broken.getInputMutable().assign(comb::MuxOp::create(
          builder, opLoc, gateEnable, lowering.orOf({}, opLoc), brokenNow,
          true));
      gatedOp.enableSince =
          lowering.andOf({op.enable, lowering.notOf(broken)}, opLoc);
    }
    if (gateEnable) {
      auto &gated = gatedLowerings[op.clock];
      if (!gated)
        gated = std::make_unique<Lowering>(
            builder, seq::ToClockOp::create(builder, loc, op.clock));
      opLowering = gated.get();
    }
    auto violation = opLowering->violation(gatedOp, reason);
    if (failed(violation)) {
      // An assertion that is asked for must be checked; a dropped assumption
      // only allows more behaviors.
      if (!isAssume && !selected.empty()) {
        op.op->emitError("unsupported assertion: ") << reason;
        return failure();
      }
      op.op->emitWarning(isAssume ? "assumption" : "assertion")
          << " left out (unsupported " << reason << ")";
      ++numSkipped;
      continue;
    }
    Value checked = *violation;
    if (gateEnable)
      checked = lowering.andOf({checked, gateEnable}, op.op->getLoc());
    (isAssume ? assumptions : violations).push_back(checked);
  }
  numAsserts += violations.size();
  numAssumes += assumptions.size();

  Value bad = lowering.orOf(violations, loc);
  if (constraints) {
    // Each assumption holds in every cycle (the trace ends where one would
    // be violated, as an AIGER constraint).
    for (auto violated : assumptions)
      verif::AssumeOp::create(builder, loc, lowering.notOf(violated), Value(),
                              StringAttr());
  } else if (!assumptions.empty()) {
    // An assumption was violated now or before: the run no longer counts.
    auto failedBefore = lowering.reg(lowering.orOf({}, loc), loc);
    SmallVector<Value> now(assumptions);
    now.push_back(failedBefore);
    Value failedNow = lowering.orOf(now, loc);
    failedBefore.getInputMutable().assign(failedNow);
    bad = lowering.andOf({bad, lowering.notOf(failedNow)}, loc);
  }

  // Remove the assertions and the LTL ops only they used.
  for (auto &op : ops)
    op.op->erase();
  SetVector<Operation *> worklist(lowering.visited.begin(),
                                  lowering.visited.end());
  for (auto &[clock, gated] : gatedLowerings)
    worklist.insert(gated->visited.begin(), gated->visited.end());
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto *op : worklist.takeVector()) {
      if (op->use_empty()) {
        op->erase();
        changed = true;
      } else {
        worklist.insert(op);
      }
    }
  }

  auto output = cast<hw::OutputOp>(body->getTerminator());
  if (replaceOutputs) {
    output->setOperands({});
    SmallVector<unsigned> outputs(module.getNumOutputPorts());
    std::iota(outputs.begin(), outputs.end(), 0);
    module.modifyPorts({}, {}, {}, outputs);
  }
  module.appendOutput(outputName, bad);
  return success();
}

void AssertionsToOutputPass::runOnOperation() {
  llvm::StringSet<> found;
  for (auto module : getOperation().getOps<hw::HWModuleOp>())
    if (failed(lower(module, found)))
      return signalPassFailure();
  for (auto &label : asserts)
    if (!found.contains(label)) {
      getOperation().emitError("no assertion labeled '") << label << "'";
      return signalPassFailure();
    }
}
