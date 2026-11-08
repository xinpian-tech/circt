//===- ClockGateToEnable.cpp - Gated clocks to clock enables --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Seq/SeqPasses.h"

namespace circt {
namespace seq {
#define GEN_PASS_DEF_CLOCKGATETOENABLE
#include "circt/Dialect/Seq/SeqPasses.h.inc"
} // namespace seq
} // namespace circt

using namespace circt;
using namespace seq;
using namespace mlir;

namespace {
struct ClockGateToEnablePass
    : public seq::impl::ClockGateToEnableBase<ClockGateToEnablePass> {
  void runOnOperation() override;
};
} // namespace

/// The ungated clock of `clock` and the enable of its gates (null if `clock`
/// is not gated).
static std::pair<Value, Value> ungate(OpBuilder &builder, Location loc,
                                      Value clock) {
  Value enable;
  while (true) {
    // Look through `to_clock(from_clock(x))`.
    if (auto toClock = clock.getDefiningOp<ToClockOp>())
      if (auto fromClock = toClock.getInput().getDefiningOp<FromClockOp>()) {
        clock = fromClock.getInput();
        continue;
      }
    auto gate = clock.getDefiningOp<ClockGateOp>();
    if (!gate)
      break;
    Value gateEnable = gate.getEnable();
    if (auto testEnable = gate.getTestEnable())
      gateEnable =
          comb::OrOp::create(builder, loc, gateEnable, testEnable, true);
    enable = enable ? comb::AndOp::create(builder, loc, enable, gateEnable,
                                          true)
                          .getResult()
                    : gateEnable;
    clock = gate.getInput();
  }
  return {clock, enable};
}

void ClockGateToEnablePass::runOnOperation() {
  SmallVector<Operation *> regs;
  getOperation().walk([&](Operation *op) {
    if (isa<FirRegOp, CompRegOp, CompRegClockEnabledOp>(op))
      regs.push_back(op);
  });

  for (auto *op : regs) {
    OpBuilder builder(op);
    auto loc = op->getLoc();
    auto gateReset = [&](Value reset, Value enable) {
      return comb::AndOp::create(builder, loc, reset, enable, true);
    };
    if (auto reg = dyn_cast<FirRegOp>(op)) {
      auto [clock, enable] = ungate(builder, loc, reg.getClk());
      if (!enable)
        continue;
      reg.getClkMutable().assign(clock);
      reg.getNextMutable().assign(comb::MuxOp::create(
          builder, loc, enable, reg.getNext(), reg.getResult(), true));
      if (reg.getReset() && !reg.getIsAsync())
        reg.getResetMutable().assign(gateReset(reg.getReset(), enable));
    } else if (auto reg = dyn_cast<CompRegOp>(op)) {
      auto [clock, enable] = ungate(builder, loc, reg.getClk());
      if (!enable)
        continue;
      reg.getClkMutable().assign(clock);
      reg.getInputMutable().assign(comb::MuxOp::create(
          builder, loc, enable, reg.getInput(), reg.getResult(), true));
      if (reg.getReset())
        reg.getResetMutable().assign(gateReset(reg.getReset(), enable));
    } else if (auto reg = dyn_cast<CompRegClockEnabledOp>(op)) {
      auto [clock, enable] = ungate(builder, loc, reg.getClk());
      if (!enable)
        continue;
      reg.getClkMutable().assign(clock);
      reg.getClockEnableMutable().assign(comb::AndOp::create(
          builder, loc, reg.getClockEnable(), enable, true));
      if (reg.getReset())
        reg.getResetMutable().assign(gateReset(reg.getReset(), enable));
    }
    ++numRegisters;
  }

  // Remove the gates that are no longer used (also gates of removed gates).
  for (bool changed = true; changed;) {
    changed = false;
    SmallVector<ClockGateOp> gates;
    getOperation().walk([&](ClockGateOp gate) { gates.push_back(gate); });
    for (auto gate : gates)
      if (gate->use_empty() && !gate.getInnerSymAttr()) {
        gate.erase();
        changed = true;
      }
  }
}
