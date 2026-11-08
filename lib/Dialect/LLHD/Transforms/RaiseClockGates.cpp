//===- RaiseClockGates.cpp - Latch-based clock gates to seq.clock_gate ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "circt/Dialect/LLHD/LLHDPasses.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Matchers.h"

namespace circt {
namespace llhd {
#define GEN_PASS_DEF_RAISECLOCKGATESPASS
#include "circt/Dialect/LLHD/LLHDPasses.h.inc"
} // namespace llhd
} // namespace circt

using namespace mlir;
using namespace circt;
using namespace llhd;

/// Whether `cond` is the negation of `clock`.
static bool isNegationOf(Value cond, Value clock) {
  if (auto xorOp = cond.getDefiningOp<comb::XorOp>()) {
    if (xorOp.getNumOperands() != 2)
      return false;
    for (unsigned i = 0; i < 2; ++i)
      if (xorOp.getOperand(i) == clock &&
          matchPattern(xorOp.getOperand(1 - i), m_One()))
        return true;
    return false;
  }
  if (auto icmp = cond.getDefiningOp<comb::ICmpOp>()) {
    auto pred = icmp.getPredicate();
    for (unsigned i = 0; i < 2; ++i) {
      if (icmp.getOperand(i) != clock)
        continue;
      auto other = icmp.getOperand(1 - i);
      if (pred == comb::ICmpPredicate::eq && matchPattern(other, m_Zero()))
        return true;
      if (pred == comb::ICmpPredicate::ne && matchPattern(other, m_One()))
        return true;
    }
  }
  return false;
}

/// `value` with `clock` replaced by 0 in its combinational fan-in: the copies
/// of the comb ops that depend on `clock` are created at `builder`.
static Value cofactorLow(Value value, Value clock, OpBuilder &builder,
                         DenseMap<Value, Value> &memo, Value &zero) {
  if (value == clock) {
    if (!zero)
      zero = hw::ConstantOp::create(builder, clock.getLoc(),
                                    builder.getI1Type(), 0);
    return zero;
  }
  if (auto it = memo.find(value); it != memo.end())
    return it->second;
  auto *op = value.getDefiningOp();
  if (!op || !isa<comb::CombDialect>(op->getDialect()) ||
      op->getNumRegions() != 0) {
    memo[value] = value;
    return value;
  }
  IRMapping mapping;
  bool changed = false;
  for (auto operand : op->getOperands()) {
    auto mapped = cofactorLow(operand, clock, builder, memo, zero);
    changed |= mapped != operand;
    mapping.map(operand, mapped);
  }
  Value result = value;
  if (changed)
    result = builder.clone(*op, mapping)
                 ->getResult(cast<OpResult>(value).getResultNumber());
  memo[value] = result;
  return result;
}

namespace {
struct RaiseClockGatesPass
    : public llhd::impl::RaiseClockGatesPassBase<RaiseClockGatesPass> {
  void runOnOperation() override;
};
} // namespace

void RaiseClockGatesPass::runOnOperation() {
  auto module = getOperation();
  SmallVector<comb::AndOp> ands;
  module.walk([&](comb::AndOp op) {
    if (op.getNumOperands() == 2 && op.getType().isInteger(1))
      ands.push_back(op);
  });

  for (auto andOp : ands) {
    for (unsigned i = 0; i < 2; ++i) {
      Value clock = andOp.getOperand(i);
      auto probe = andOp.getOperand(1 - i).getDefiningOp<ProbeOp>();
      if (!probe)
        continue;
      auto signal = probe.getSignal().getDefiningOp<SignalOp>();
      if (!signal)
        continue;

      // The latch: driven once, in the module body, only while the clock is
      // low, without real-time delay.
      DriveOp drive;
      bool onlyProbesAndOneDrive = true;
      for (auto *user : signal->getUsers()) {
        if (auto drv = dyn_cast<DriveOp>(user)) {
          if (drive || drv.getSignal() != signal.getResult())
            onlyProbesAndOneDrive = false;
          drive = drv;
        } else if (!isa<ProbeOp>(user)) {
          onlyProbesAndOneDrive = false;
        }
      }
      if (!onlyProbesAndOneDrive || !drive ||
          drive->getParentOp() != module.getOperation() ||
          !drive.getEnable() || !isNegationOf(drive.getEnable(), clock))
        continue;
      auto time = drive.getTime().getDefiningOp<ConstantTimeOp>();
      if (!time || time.getValue().getTime() != 0)
        continue;

      // The enable is what the latch holds at the rising edge: its input
      // while the clock is low. The gated clock `clk & latch` is the clock
      // gate's output, as a clock or as a value.
      OpBuilder builder(andOp);
      DenseMap<Value, Value> memo;
      Value zero;
      auto enable = cofactorLow(drive.getValue(), clock, builder, memo, zero);
      auto loc = andOp.getLoc();
      auto baseClock = seq::ToClockOp::create(builder, loc, clock);
      auto gate = seq::ClockGateOp::create(builder, loc, baseClock, enable);
      auto gated = seq::FromClockOp::create(builder, loc, gate);
      andOp.replaceAllUsesWith(gated.getResult());
      andOp.erase();
      if (probe->use_empty())
        probe.erase();
      ++numRaised;
      break;
    }
  }
}
