//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the TrimRegisters pass: a cone of influence at the
// granularity of bits. Starting from the module outputs and the operations
// that observe their operands by themselves (assertions, assumptions and
// anything unknown), it propagates which bits of each value can be observed,
// through extracts, concatenations, replications, bitwise operators and mux
// data inputs bit by bit, and through registers from their result to their
// next state. Register bits that are never observed are removed: the register
// is narrowed to its observed bits and the removed bits read as zero, which
// no observer can tell apart. A register bit that only feeds itself, such as
// a byte of a holding register whose readers were shown not to look at it,
// is removed even though the register as a whole is live.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Seq/SeqPasses.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace circt {
namespace seq {
#define GEN_PASS_DEF_TRIMREGISTERS
#include "circt/Dialect/Seq/SeqPasses.h.inc"
} // namespace seq
} // namespace circt

using namespace mlir;
using namespace circt;
using namespace seq;

namespace {

/// Width of the observation mask of a value: its bits for integers, one
/// "observed" bit for anything else.
static unsigned maskWidth(Value v) {
  if (auto type = dyn_cast<IntegerType>(v.getType()))
    return type.getWidth();
  return 1;
}

struct TrimRegistersPass
    : public seq::impl::TrimRegistersBase<TrimRegistersPass> {
  void runOnOperation() override;

private:
  /// Adds `bits` to the observed bits of `v`.
  void observe(Value v, const APInt &bits);
  /// Propagates the observed bits of `v` to the operands of its definition.
  void propagate(Value v);
  bool trim(CompRegOp reg);

  DenseMap<Value, APInt> observed;
  SmallVector<Value> worklist;
};

void TrimRegistersPass::observe(Value v, const APInt &bits) {
  if (bits.isZero())
    return;
  auto [it, inserted] =
      observed.try_emplace(v, APInt(maskWidth(v), 0));
  APInt merged = it->second | bits;
  if (!inserted && merged == it->second)
    return;
  it->second = merged;
  worklist.push_back(v);
}

void TrimRegistersPass::propagate(Value v) {
  auto *op = v.getDefiningOp();
  if (!op)
    return;
  APInt bits = observed.lookup(v);
  auto all = [&](Value operand) {
    observe(operand, APInt::getAllOnes(maskWidth(operand)));
  };
  if (auto extract = dyn_cast<comb::ExtractOp>(op)) {
    Value input = extract.getInput();
    observe(input, bits.zext(maskWidth(input)).shl(extract.getLowBit()));
    return;
  }
  if (auto concat = dyn_cast<comb::ConcatOp>(op)) {
    unsigned pos = maskWidth(v);
    for (Value operand : concat.getInputs()) {
      unsigned width = maskWidth(operand);
      pos -= width;
      observe(operand, bits.extractBits(width, pos));
    }
    return;
  }
  if (auto replicate = dyn_cast<comb::ReplicateOp>(op)) {
    Value input = replicate.getInput();
    unsigned width = maskWidth(input);
    APInt merged(width, 0);
    for (unsigned pos = 0; pos < maskWidth(v); pos += width)
      merged |= bits.extractBits(width, pos);
    observe(input, merged);
    return;
  }
  if (isa<comb::AndOp, comb::OrOp, comb::XorOp>(op)) {
    for (Value operand : op->getOperands())
      observe(operand, bits);
    return;
  }
  if (auto mux = dyn_cast<comb::MuxOp>(op)) {
    if (isa<IntegerType>(v.getType())) {
      observe(mux.getTrueValue(), bits);
      observe(mux.getFalseValue(), bits);
      all(mux.getCond());
      return;
    }
  }
  // A carry only moves up: the low bits of the operands up to the highest
  // observed bit of the result.
  if (isa<comb::AddOp, comb::SubOp, comb::MulOp>(op)) {
    APInt low = APInt::getLowBitsSet(bits.getBitWidth(), bits.getActiveBits());
    for (Value operand : op->getOperands())
      observe(operand, low);
    return;
  }
  if (auto reg = dyn_cast<CompRegOp>(op)) {
    if (isa<IntegerType>(v.getType())) {
      observe(reg.getInput(), bits);
      if (reg.getResetValue())
        observe(reg.getResetValue(), bits);
    } else {
      all(reg.getInput());
      if (reg.getResetValue())
        all(reg.getResetValue());
    }
    all(reg.getClk());
    if (reg.getReset())
      all(reg.getReset());
    if (reg.getInitialValue())
      all(reg.getInitialValue());
    return;
  }
  // Anything else observes all of its operands.
  for (Value operand : op->getOperands())
    all(operand);
}

/// The constant initial value of a register, if it has one.
static std::optional<APInt> constantInitial(CompRegOp reg) {
  auto initial = reg.getInitialValue();
  if (!initial || !initial.getDefiningOp<InitialOp>())
    return std::nullopt;
  APInt value;
  if (!matchPattern(unwrapImmutableValue(initial), m_ConstantInt(&value)))
    return std::nullopt;
  return value;
}

bool TrimRegistersPass::trim(CompRegOp reg) {
  auto type = dyn_cast<IntegerType>(reg.getType());
  if (!type || reg.getInnerSymAttr())
    return false;
  unsigned width = type.getWidth();
  APInt keep = observed.lookup(reg.getData());
  if (keep.getBitWidth() != width)
    keep = APInt(width, 0);
  if (keep.isAllOnes())
    return false;
  std::optional<APInt> init;
  if (reg.getInitialValue()) {
    init = constantInitial(reg);
    if (!init)
      return false;
  }

  OpBuilder builder(reg);
  Location loc = reg.getLoc();
  unsigned newWidth = keep.popcount();
  Value data;
  if (newWidth == 0) {
    data = hw::ConstantOp::create(builder, loc, APInt(width, 0));
  } else {
    // Runs of kept and removed bits, most significant first.
    SmallVector<std::pair<unsigned, unsigned>> runs; // (low bit, width)
    for (int hi = width; hi > 0;) {
      bool kept = keep[hi - 1];
      int lo = hi - 1;
      while (lo > 0 && keep[lo - 1] == kept)
        --lo;
      runs.push_back({unsigned(lo), unsigned(hi - lo)});
      hi = lo;
    }
    auto gather = [&](Value value) {
      SmallVector<Value> parts;
      for (auto [lo, len] : runs)
        if (keep[lo])
          parts.push_back(builder.createOrFold<comb::ExtractOp>(
              loc, builder.getIntegerType(len), value, lo));
      return parts.size() == 1
                 ? parts.front()
                 : builder.createOrFold<comb::ConcatOp>(loc, parts);
    };
    Value input = gather(reg.getInput());
    Value resetValue = reg.getResetValue() ? gather(reg.getResetValue())
                                           : Value();
    Value initial;
    if (init) {
      APInt newInit(newWidth, 0);
      unsigned pos = newWidth;
      for (auto [lo, len] : runs)
        if (keep[lo]) {
          pos -= len;
          newInit.insertBits(init->extractBits(len, lo), pos);
        }
      initial = createConstantInitialValue(
          builder, loc, builder.getIntegerAttr(builder.getIntegerType(newWidth),
                                               newInit));
    }
    auto newReg = CompRegOp::create(builder, loc, input.getType(), input,
                                    reg.getClk(), reg.getNameAttr(),
                                    reg.getReset(), resetValue, initial,
                                    hw::InnerSymAttr());
    SmallVector<Value> parts;
    unsigned pos = newWidth;
    for (auto [lo, len] : runs) {
      if (keep[lo]) {
        pos -= len;
        parts.push_back(builder.createOrFold<comb::ExtractOp>(
            loc, builder.getIntegerType(len), newReg.getData(), pos));
      } else {
        parts.push_back(hw::ConstantOp::create(builder, loc, APInt(len, 0)));
      }
    }
    data = parts.size() == 1 ? parts.front()
                             : builder.createOrFold<comb::ConcatOp>(loc, parts);
  }
  reg.getData().replaceAllUsesWith(data);
  reg.erase();
  ++numTrimmedRegisters;
  numRemovedBits += width - newWidth;
  return true;
}

void TrimRegistersPass::runOnOperation() {
  observed.clear();
  worklist.clear();
  Block *body = getOperation().getBodyBlock();
  // Roots: everything that is not a value-to-value operator the propagation
  // knows, and is not a register (a register is observed through its result).
  for (auto &op : *body) {
    if (isa<comb::ExtractOp, comb::ConcatOp, comb::ReplicateOp, comb::AndOp,
            comb::OrOp, comb::XorOp, comb::MuxOp, comb::AddOp, comb::SubOp,
            comb::MulOp, CompRegOp, hw::ConstantOp>(op))
      continue;
    if (op.getNumResults() != 0 && isMemoryEffectFree(&op) &&
        op.getNumRegions() == 0)
      continue;
    op.walk([&](Operation *inner) {
      for (Value operand : inner->getOperands())
        if (operand.getParentBlock() == body)
          observe(operand, APInt::getAllOnes(maskWidth(operand)));
    });
  }
  while (!worklist.empty())
    propagate(worklist.pop_back_val());

  SmallVector<CompRegOp> regs;
  for (auto reg : body->getOps<CompRegOp>())
    regs.push_back(reg);
  bool changed = false;
  for (auto reg : regs)
    changed |= trim(reg);
  if (!changed)
    markAllAnalysesPreserved();
}

} // namespace
