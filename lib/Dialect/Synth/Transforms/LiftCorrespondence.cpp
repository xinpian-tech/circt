//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the LiftCorrespondence pass: signal correspondence
// proved on bits, applied to words. A copy of each module gets every word
// value (register or combinational operation) as an extra output and is
// bit-blasted and reduced by signal correspondence (`pipeline`, by default
// the bit-level lowering and synth-register-correspondence{internal}). In the
// reduced copy, two word values whose outputs are the same bits are equal in
// every reachable state, and an output bit that is a constant is constant:
// these are merged in the original, word-level module, which keeps its words
// for word-level back ends (BTOR2, SMT) and for later word-level passes.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Synth/SynthOps.h"
#include "circt/Dialect/Synth/Transforms/SynthPasses.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassRegistry.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

namespace circt {
namespace synth {
#define GEN_PASS_DEF_LIFTCORRESPONDENCE
#include "circt/Dialect/Synth/Transforms/SynthPasses.h.inc"
} // namespace synth
} // namespace circt

using namespace mlir;
using namespace circt;

namespace {
struct LiftCorrespondencePass
    : public synth::impl::LiftCorrespondenceBase<LiftCorrespondencePass> {
  using LiftCorrespondenceBase::LiftCorrespondenceBase;
  void runOnOperation() override;
  void getDependentDialects(DialectRegistry &registry) const override {
    LiftCorrespondenceBase::getDependentDialects(registry);
    OpPassManager pm(hw::HWModuleOp::getOperationName());
    if (succeeded(parsePassPipeline(pipeline, pm)))
      pm.getDependentDialects(registry);
  }
  LogicalResult lift(hw::HWModuleOp module);
};
} // namespace

/// The bits of a value of the reduced copy, most significant first, looking
/// through concatenations.
static void bitsOf(Value v, SmallVectorImpl<Value> &bits) {
  if (auto concat = v.getDefiningOp<comb::ConcatOp>()) {
    for (auto operand : concat.getInputs())
      bitsOf(operand, bits);
    return;
  }
  auto width = cast<IntegerType>(v.getType()).getWidth();
  if (width == 1) {
    bits.push_back(v);
    return;
  }
  // A constant word: one i1 constant per bit (in the copy, erased later).
  if (auto c = v.getDefiningOp<hw::ConstantOp>()) {
    OpBuilder b(c);
    for (int k = width - 1; k >= 0; --k)
      bits.push_back(hw::ConstantOp::create(b, c.getLoc(),
                                            APInt(1, c.getValue()[k])));
    return;
  }
  // A replicated bit.
  if (auto r = v.getDefiningOp<comb::ReplicateOp>()) {
    SmallVector<Value> one;
    bitsOf(r.getInput(), one);
    for (unsigned k = 0; k < width / one.size(); ++k)
      bits.append(one);
    return;
  }
  // A word that is not a concatenation: its bits as extracts (compared by
  // the word and position).
  for (int k = width - 1; k >= 0; --k)
    bits.push_back(Value()); // unknown bit identity: never merged
}

LogicalResult LiftCorrespondencePass::lift(hw::HWModuleOp module) {
  Block *body = module.getBodyBlock();
  // The candidates: integer results of registers and side-effect free
  // combinational operations.
  SmallVector<Value> cands;
  for (auto &op : *body) {
    if (op.getNumResults() != 1 || !isa<IntegerType>(op.getResultTypes()[0]))
      continue;
    if (isa<hw::ConstantOp>(op))
      continue;
    if (isa<seq::CompRegOp>(op) || isa<comb::CombDialect>(op.getDialect()))
      cands.push_back(op.getResult(0));
  }
  if (cands.empty())
    return success();

  // The copy, with the candidates as extra outputs, next to the module.
  OpBuilder builder(module);
  auto copy = cast<hw::HWModuleOp>(builder.clone(*module));
  SymbolTable::setSymbolName(copy, module.getName().str() + "_lift_copy");
  {
    IRMapping mapping;
    // Map the original values to the copy's by walking both bodies.
    auto &from = *body, &to = *copy.getBodyBlock();
    for (auto [a, b] : llvm::zip(from.getArguments(), to.getArguments()))
      mapping.map(a, b);
    for (auto [a, b] : llvm::zip(from, to))
      for (auto [ra, rb] : llvm::zip(a.getResults(), b.getResults()))
        mapping.map(ra, rb);
    SmallVector<std::pair<StringAttr, Value>> outputs;
    for (auto [k, v] : llvm::enumerate(cands))
      outputs.push_back({builder.getStringAttr("lift_" + std::to_string(k)),
                         mapping.lookup(v)});
    copy.appendOutputs(outputs);
  }
  unsigned numOriginal = module.getNumOutputPorts();

  OpPassManager pm(hw::HWModuleOp::getOperationName());
  if (failed(parsePassPipeline(pipeline, pm))) {
    copy.erase();
    return module.emitError("cannot parse pipeline: ") << pipeline;
  }
  if (failed(runPipeline(pm, copy))) {
    copy.erase();
    return failure();
  }

  // Group the candidates by their bits in the reduced copy.
  auto *term = copy.getBodyBlock()->getTerminator();
  std::map<SmallVector<void *>, SmallVector<unsigned>> groups;
  SmallVector<SmallVector<Value>> candBits(cands.size());
  for (unsigned k = 0; k < cands.size(); ++k) {
    Value v = term->getOperand(numOriginal + k);
    bitsOf(v, candBits[k]);
    SmallVector<void *> key;
    bool known = true;
    for (Value b : candBits[k]) {
      if (!b) {
        known = false;
        break;
      }
      // Constants by value (different constant ops of one value are equal).
      if (auto c = b.getDefiningOp<hw::ConstantOp>())
        key.push_back(reinterpret_cast<void *>(c.getValue().getZExtValue() + 1));
      else
        key.push_back(b.getAsOpaquePointer());
    }
    if (known)
      groups[key].push_back(k);
  }

  // Ranks: registers first, then operations in a post order of their
  // combinational operands, so a member is replaced by one that comes
  // before it and does not read it.
  DenseMap<Value, unsigned> rank;
  unsigned next = 0;
  for (auto reg : body->getOps<seq::CompRegOp>())
    rank[reg.getResult()] = next++;
  DenseSet<Operation *> visiting;
  std::function<void(Operation *)> visit = [&](Operation *op) {
    if (!op || op->getBlock() != body || isa<seq::CompRegOp>(op) ||
        op->getNumResults() != 1 || rank.count(op->getResult(0)) ||
        !visiting.insert(op).second)
      return;
    for (auto operand : op->getOperands())
      visit(operand.getDefiningOp());
    rank[op->getResult(0)] = next++;
  };
  for (auto &op : *body)
    visit(&op);
  // Does the combinational cone of `root` read `target`?
  auto reads = [&](Value root, Value target) {
    SmallVector<Value> stack{root};
    DenseSet<Value> seen;
    while (!stack.empty()) {
      Value v = stack.pop_back_val();
      if (v == target)
        return true;
      if (!seen.insert(v).second)
        continue;
      Operation *op = v.getDefiningOp();
      if (!op || isa<seq::CompRegOp>(op))
        continue;
      for (auto operand : op->getOperands())
        stack.push_back(operand);
    }
    return false;
  };

  // Merge: constant groups to constants, the others into the member of
  // least rank.
  OpBuilder b(module.getContext());
  b.setInsertionPoint(body->getTerminator());
  DenseSet<unsigned> merged;
  unsigned mergedValues = 0, constValues = 0;
  for (auto &[key, members] : groups) {
    bool allConst = llvm::all_of(candBits[members[0]], [](Value v) {
      return v.getDefiningOp<hw::ConstantOp>();
    });
    if (allConst) {
      APInt value(cands[members[0]].getType().getIntOrFloatBitWidth(), 0);
      unsigned pos = value.getBitWidth();
      for (Value bit : candBits[members[0]]) {
        --pos;
        if (bit.getDefiningOp<hw::ConstantOp>().getValue().getBoolValue())
          value.setBit(pos);
      }
      Value c = hw::ConstantOp::create(b, module.getLoc(), value);
      for (unsigned k : members) {
        cands[k].replaceAllUsesWith(c);
        merged.insert(k);
        ++constValues;
      }
      continue;
    }
    if (members.size() < 2)
      continue;
    SmallVector<unsigned> sorted(members);
    llvm::sort(sorted, [&](unsigned x, unsigned y) {
      return rank.lookup(cands[x]) < rank.lookup(cands[y]);
    });
    Value repr = cands[sorted[0]];
    for (unsigned k : llvm::drop_begin(sorted)) {
      if (reads(repr, cands[k]))
        continue;
      cands[k].replaceAllUsesWith(repr);
      merged.insert(k);
      ++mergedValues;
    }
  }
  // Constant bits of the other candidates.
  unsigned constBits = 0;
  for (unsigned k = 0; k < cands.size(); ++k) {
    if (merged.contains(k))
      continue;
    auto &bits = candBits[k];
    if (llvm::any_of(bits, [](Value v) { return !v; }) ||
        llvm::none_of(bits, [](Value v) {
          return v.getDefiningOp<hw::ConstantOp>();
        }))
      continue;
    Value v = cands[k];
    unsigned width = bits.size();
    b.setInsertionPointAfterValue(v);
    SmallVector<Value> parts;
    SmallPtrSet<Operation *, 8> created;
    // Runs of constant and other bits, most significant first.
    for (unsigned i = 0; i < width;) {
      bool isConst = bool(bits[i].getDefiningOp<hw::ConstantOp>());
      unsigned j = i;
      while (j < width &&
             bool(bits[j].getDefiningOp<hw::ConstantOp>()) == isConst)
        ++j;
      unsigned len = j - i, lo = width - j;
      if (isConst) {
        APInt c(len, 0);
        for (unsigned t = 0; t < len; ++t)
          if (bits[i + t].getDefiningOp<hw::ConstantOp>().getValue().getBoolValue())
            c.setBit(len - 1 - t);
        parts.push_back(hw::ConstantOp::create(b, v.getLoc(), c));
        constBits += len;
      } else {
        auto e = comb::ExtractOp::create(b, v.getLoc(), v, lo, len);
        parts.push_back(e);
        created.insert(e);
      }
      i = j;
    }
    auto concat = comb::ConcatOp::create(b, v.getLoc(), parts);
    v.replaceAllUsesExcept(concat, created);
  }
  copy.erase();
  numMergedValues += mergedValues;
  numConstantValues += constValues;
  numConstantBits += constBits;
  numCandidates += cands.size();
  return success();
}

void LiftCorrespondencePass::runOnOperation() {
  SmallVector<hw::HWModuleOp> modules(getOperation().getOps<hw::HWModuleOp>());
  for (auto module : modules)
    if (failed(lift(module)))
      return signalPassFailure();
}
