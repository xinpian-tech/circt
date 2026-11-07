//===- LowerFirRegToCompReg.cpp - seq.firreg to seq.compreg --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lower `seq.firreg` to `seq.compreg` without reset and with an explicit
// initial value, for formal backends that only take plain registers with an
// initial state (AIGER latches):
//
//   reset (sync)     compreg(mux(rst, rv, next))
//   reset (async)    the same, read as mux(rst, rv, latch): the reset value is
//                    visible in the cycle where the reset is asserted
//   initial value    the reset value if constant and `initial-reset`; a
//                    non-constant reset value is taken in the first cycle
//                    (mux(first, latch, rv)); the preset if there is one;
//                    else any value (a `verif.symbolic_value` in the first
//                    cycle), as firreg registers start randomized.
//
// Aggregate registers are lowered on their bits (hw.bitcast).
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/HW/HWTypes.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Seq/SeqPasses.h"
#include "circt/Dialect/Verif/VerifOps.h"
#include "mlir/IR/Matchers.h"

using namespace circt;
using namespace seq;
using namespace mlir;

namespace circt {
namespace seq {
#define GEN_PASS_DEF_LOWERFIRREGTOCOMPREG
#include "circt/Dialect/Seq/SeqPasses.h.inc"
} // namespace seq
} // namespace circt

namespace {
struct LowerFirRegToCompRegPass
    : public seq::impl::LowerFirRegToCompRegBase<LowerFirRegToCompRegPass> {
  using LowerFirRegToCompRegBase::LowerFirRegToCompRegBase;
  void runOnOperation() override;
};
} // namespace

void LowerFirRegToCompRegPass::runOnOperation() {
  auto module = getOperation();
  SmallVector<FirRegOp> regs;
  module.walk([&](FirRegOp op) { regs.push_back(op); });
  if (regs.empty())
    return;

  // A register that is 0 in the first cycle and 1 afterwards, per clock.
  DenseMap<Value, Value> firsts;
  DenseMap<Attribute, Value> initials;
  auto initial = [&](OpBuilder &builder, Location loc, IntegerAttr attr) {
    auto &result = initials[attr];
    if (!result) {
      OpBuilder::InsertionGuard guard(builder);
      builder.setInsertionPointToStart(module.getBodyBlock());
      result = createConstantInitialValue(builder, loc, attr);
    }
    return result;
  };
  auto first = [&](OpBuilder &builder, Location loc, Value clock) {
    auto &result = firsts[clock];
    if (!result) {
      OpBuilder::InsertionGuard guard(builder);
      builder.setInsertionPointToStart(module.getBodyBlock());
      auto i1 = builder.getI1Type();
      Value one = hw::ConstantOp::create(builder, loc, i1, 1);
      result = CompRegOp::create(
          builder, loc, one, clock, Value(), Value(),
          builder.getStringAttr("first"), hw::InnerSymAttr(),
          initial(builder, loc, builder.getIntegerAttr(i1, 0)));
    }
    return result;
  };

  for (auto reg : regs) {
    OpBuilder builder(reg);
    auto loc = reg.getLoc();
    Type type = reg.getType();
    auto width = hw::getBitWidth(type);
    if (width < 0) {
      reg.emitError("register of unknown bit width");
      return signalPassFailure();
    }
    auto intType = builder.getIntegerType(width);
    bool aggregate = type != intType;
    auto toInt = [&](Value value) -> Value {
      return aggregate ? hw::BitcastOp::create(builder, loc, intType, value)
                       : value;
    };
    Value next = toInt(reg.getNext());
    Value clock = reg.getClk();
    Value reset = reg.getReset();
    Value resetValue = reset ? toInt(reg.getResetValue()) : Value();
    if (reset)
      next = comb::MuxOp::create(builder, loc, reset, resetValue, next, true);

    // The initial state: a constant, or a value taken in the first cycle.
    IntegerAttr init;
    Value firstValue;
    APInt c;
    if (reset && initialReset && matchPattern(resetValue, m_ConstantInt(&c))) {
      init = builder.getIntegerAttr(intType, c);
      ++numConstReset;
    } else if (reset && initialReset) {
      firstValue = resetValue;
      ++numValueReset;
    } else if (auto preset = reg.getPresetAttr()) {
      init = builder.getIntegerAttr(intType, preset.getValue());
      ++numPreset;
    } else if (freeInitial) {
      firstValue =
          verif::SymbolicValueOp::create(builder, loc, intType, StringAttr());
      ++numFree;
    } else {
      init = builder.getIntegerAttr(intType, 0);
      ++numZero;
    }
    if (!init)
      init = builder.getIntegerAttr(intType, 0);
    Value value = CompRegOp::create(builder, loc, next, clock, Value(), Value(),
                                    reg.getNameAttr(), reg.getInnerSymAttr(),
                                    initial(builder, loc, init));
    if (firstValue)
      value = comb::MuxOp::create(builder, loc, first(builder, loc, clock),
                                  value, firstValue, true);
    if (reset && reg.getIsAsync())
      value = comb::MuxOp::create(builder, loc, reset, resetValue, value, true);
    if (aggregate)
      value = hw::BitcastOp::create(builder, loc, type, value);
    reg.replaceAllUsesWith(value);
    reg.erase();
  }
}
