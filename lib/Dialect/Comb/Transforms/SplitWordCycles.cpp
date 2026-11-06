//===- SplitWordCycles.cpp - Split word-level combinational cycles --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A word-level combinational cycle need not be a cycle of bits: if a field of
// a struct built in one `always_comb` depends on another field of it, the
// struct (or the integer it is lowered to) depends on itself, but no bit does.
// Passes that order a module's logic topologically at word level reject such
// modules. This pass pushes `comb.extract` through the bitwise operations of
// a cycle (`mux`, `and`, `or`, `xor`, `concat`, `replicate`), so that the
// cycle splits into the parts that are actually used, and repeats until no
// cycle is left or no extract can be pushed further. Logic outside of cycles
// is not touched.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/Comb/CombPasses.h"
#include "circt/Dialect/HW/HWOps.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

using namespace circt;
using namespace comb;
using namespace mlir;

namespace circt {
namespace comb {
#define GEN_PASS_DEF_SPLITWORDCYCLES
#include "circt/Dialect/Comb/Passes.h.inc"
} // namespace comb
} // namespace circt

namespace {
struct SplitWordCyclesPass
    : public comb::impl::SplitWordCyclesBase<SplitWordCyclesPass> {
  using SplitWordCyclesBase::SplitWordCyclesBase;
  void runOnOperation() override;
};
} // namespace

/// Ops that may form a combinational cycle: side-effect free ops without
/// regions of the Comb and HW dialects (registers break cycles).
static bool isCombinational(Operation *op) {
  return op && op->getNumRegions() == 0 &&
         isa<CombDialect, hw::HWDialect>(op->getDialect()) &&
         !isa<hw::InstanceOp, hw::OutputOp>(op);
}

/// The ops of `block` that are part of a combinational cycle.
static DenseSet<Operation *> findCycles(Block &block) {
  // Tarjan's algorithm over the operand graph of the combinational ops.
  DenseMap<Operation *, unsigned> index, low;
  DenseSet<Operation *> onStack, inCycle;
  SmallVector<Operation *> stack;
  unsigned next = 0;
  auto operands = [](Operation *op) {
    SmallVector<Operation *> defs;
    for (auto operand : op->getOperands())
      if (auto *def = operand.getDefiningOp(); isCombinational(def))
        defs.push_back(def);
    return defs;
  };
  for (auto &root : block) {
    if (!isCombinational(&root) || index.count(&root))
      continue;
    SmallVector<std::pair<Operation *, unsigned>> work{{&root, 0}};
    while (!work.empty()) {
      auto [op, child] = work.back();
      if (child == 0) {
        index[op] = low[op] = next++;
        stack.push_back(op);
        onStack.insert(op);
      }
      auto defs = operands(op);
      if (child < defs.size()) {
        work.back().second++;
        auto *def = defs[child];
        if (!index.count(def))
          work.push_back({def, 0});
        else if (onStack.contains(def))
          low[op] = std::min(low[op], index[def]);
        continue;
      }
      work.pop_back();
      if (!work.empty())
        low[work.back().first] = std::min(low[work.back().first], low[op]);
      if (low[op] != index[op])
        continue;
      SmallVector<Operation *> scc;
      Operation *member;
      do {
        member = stack.pop_back_val();
        onStack.erase(member);
        scc.push_back(member);
      } while (member != op);
      bool selfLoop = llvm::is_contained(operands(op), op);
      if (scc.size() > 1 || selfLoop)
        inCycle.insert(scc.begin(), scc.end());
    }
  }
  return inCycle;
}

/// Push `extract` one step into its input. Returns the replacement, or null.
static Value pushExtract(ExtractOp extract, OpBuilder &builder) {
  auto *input = extract.getInput().getDefiningOp();
  auto loc = extract.getLoc();
  unsigned lowBit = extract.getLowBit();
  unsigned width = extract.getType().getIntOrFloatBitWidth();
  auto narrow = [&](Value value) {
    return builder.createOrFold<ExtractOp>(loc, value, lowBit, width);
  };
  if (auto mux = dyn_cast_or_null<MuxOp>(input))
    return MuxOp::create(builder, loc, mux.getCond(),
                         narrow(mux.getTrueValue()),
                         narrow(mux.getFalseValue()), mux.getTwoState());
  if (isa_and_nonnull<AndOp, OrOp, XorOp>(input)) {
    SmallVector<Value> operands;
    for (auto operand : input->getOperands())
      operands.push_back(narrow(operand));
    OperationState state(loc, input->getName(), operands, {extract.getType()},
                         input->getAttrs());
    return builder.create(state)->getResult(0);
  }
  if (auto concat = dyn_cast_or_null<ConcatOp>(input)) {
    // Collect the parts covering [lowBit, lowBit + width).
    SmallVector<Value> parts;
    unsigned position = concat.getType().getIntOrFloatBitWidth();
    for (auto operand : concat.getInputs()) {
      unsigned operandWidth = operand.getType().getIntOrFloatBitWidth();
      unsigned operandLow = position - operandWidth;
      unsigned from = std::max(lowBit, operandLow);
      unsigned to = std::min(lowBit + width, position);
      if (from < to)
        parts.push_back(builder.createOrFold<ExtractOp>(
            loc, operand, from - operandLow, to - from));
      position = operandLow;
    }
    return parts.size() == 1 ? parts[0]
                             : builder.createOrFold<ConcatOp>(loc, parts);
  }
  if (auto replicate = dyn_cast_or_null<ReplicateOp>(input)) {
    auto element = replicate.getInput();
    if (element.getType().getIntOrFloatBitWidth() == 1)
      return width == 1 ? element
                        : builder.createOrFold<ReplicateOp>(
                              loc, extract.getType(), element);
  }
  return {};
}

void SplitWordCyclesPass::runOnOperation() {
  auto &block = *getOperation().getBodyBlock();
  for (unsigned iteration = 0; iteration < maxIterations; ++iteration) {
    auto inCycle = findCycles(block);
    if (inCycle.empty())
      return;
    bool changed = false;
    DenseSet<Operation *> erased;
    SmallVector<ExtractOp> extracts;
    for (auto *op : inCycle)
      if (auto extract = dyn_cast<ExtractOp>(op))
        if (inCycle.contains(extract.getInput().getDefiningOp()))
          extracts.push_back(extract);
    for (auto extract : extracts) {
      OpBuilder builder(extract);
      if (auto replacement = pushExtract(extract, builder)) {
        extract.replaceAllUsesWith(replacement);
        erased.insert(extract);
        extract.erase();
        ++numPushed;
        changed = true;
      }
    }
    // Remove the wide ops left without users.
    for (auto *op : inCycle)
      if (!erased.contains(op) && op->use_empty() && isMemoryEffectFree(op))
        op->erase();
    if (!changed)
      break;
  }
  auto left = findCycles(block);
  if (left.empty())
    return;

  // Optionally cut the remaining cycles: replace the operands of one op per
  // cycle that come from the cycle by new module inputs. This over-approximates
  // the module (the input may take any value), which keeps proofs of safety
  // properties valid but may admit spurious counterexamples.
  if (cutRemaining) {
    auto module = getOperation();
    for (unsigned round = 0; !left.empty() && round < 1000; ++round) {
      Operation *cut = nullptr;
      for (auto &op : block)
        if (left.contains(&op)) {
          cut = &op;
          break;
        }
      for (auto &operand : cut->getOpOperands()) {
        if (!left.contains(operand.get().getDefiningOp()))
          continue;
        auto name = StringAttr::get(&getContext(),
                                    "cycle_cut_" + Twine(numCuts.getValue()));
        auto [port, arg] = module.appendInput(name, operand.get().getType());
        (void)port;
        operand.set(arg);
        ++numCuts;
      }
      module.emitWarning("cut combinational cycle at ")
          << cut->getName() << " (" << cut->getLoc() << ")";
      left = findCycles(block);
    }
    if (left.empty())
      return;
  }
  auto diag =
      getOperation().emitWarning("combinational cycle left after splitting");
  unsigned shown = 0;
  for (auto &op : block)
    if (left.contains(&op) && shown++ < 16)
      diag.attachNote(op.getLoc()) << "part of the cycle: " << op.getName();
}
