//===- AssumeInputs.cpp - Split assumptions, substitute assumed inputs ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Simplifications that use the assumptions of a module (`verif.assume` of an
// i1 without enable, which holds in every cycle):
// - verif-split-assumes: an assumed conjunction becomes one assumption per
//   operand.
// - verif-substitute-assumed-inputs: an input bit that the assumptions
//   determine (x == 0, x == 1, x == f) is replaced by its value everywhere
//   except in the assumptions that determine it.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Synth/SynthOps.h"
#include "circt/Dialect/Verif/VerifOps.h"
#include "circt/Dialect/Verif/VerifPasses.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Matchers.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"

namespace circt {
namespace verif {
#define GEN_PASS_DEF_SPLITASSUMESPASS
#define GEN_PASS_DEF_SUBSTITUTEASSUMEDINPUTSPASS
#include "circt/Dialect/Verif/Passes.h.inc"
} // namespace verif
} // namespace circt

using namespace circt;
using namespace mlir;

/// An assumption the passes handle: of an i1, without enable, in the module
/// body.
static bool isPlainAssume(verif::AssumeOp op, Block *body) {
  auto type = dyn_cast<IntegerType>(op.getProperty().getType());
  return op->getBlock() == body && !op.getEnable() && type &&
         type.getWidth() == 1;
}

//===----------------------------------------------------------------------===//
// verif-split-assumes
//===----------------------------------------------------------------------===//

namespace {
struct SplitAssumesPass
    : public circt::verif::impl::SplitAssumesPassBase<SplitAssumesPass> {
  void runOnOperation() override;
};
} // namespace

void SplitAssumesPass::runOnOperation() {
  auto module = getOperation();
  Block *body = module.getBodyBlock();
  SmallVector<verif::AssumeOp> worklist;
  DenseSet<Value> assumed;
  for (auto op : body->getOps<verif::AssumeOp>())
    if (isPlainAssume(op, body)) {
      worklist.push_back(op);
      assumed.insert(op.getProperty());
    }
  bool changed = false;
  while (!worklist.empty()) {
    auto op = worklist.pop_back_val();
    Value p = op.getProperty();
    OpBuilder builder(op);
    auto assume = [&](Value v) {
      if (!assumed.insert(v).second)
        return;
      worklist.push_back(verif::AssumeOp::create(builder, op.getLoc(), v,
                                                 Value(), op.getLabelAttr()));
    };
    if (matchPattern(p, m_One())) {
      op.erase();
      changed = true;
      continue;
    }
    if (auto andOp = p.getDefiningOp<comb::AndOp>()) {
      for (auto in : andOp.getInputs())
        assume(in);
    } else if (auto andOp = p.getDefiningOp<synth::aig::AndInverterOp>();
               andOp && andOp.getInputs().size() > 1) {
      for (auto [in, inv] : llvm::zip(andOp.getInputs(), andOp.getInverted()))
        assume(inv ? synth::aig::AndInverterOp::create(builder, op.getLoc(), in,
                                                       true)
                   : in);
    } else {
      continue;
    }
    ++numSplit;
    op.erase();
    changed = true;
  }
  if (!changed)
    markAllAnalysesPreserved();
}

//===----------------------------------------------------------------------===//
// verif-substitute-assumed-inputs
//===----------------------------------------------------------------------===//

namespace {
/// A literal: a value, possibly negated.
struct Lit {
  Value value;
  bool neg = false;
  Lit operator!() const { return {value, !neg}; }
  bool operator==(const Lit &o) const {
    return value == o.value && neg == o.neg;
  }
};

/// A bit of a module input: the input and the bit.
using InputBit = std::pair<BlockArgument, unsigned>;

struct SubstituteAssumedInputsPass
    : public circt::verif::impl::SubstituteAssumedInputsPassBase<
          SubstituteAssumedInputsPass> {
  void runOnOperation() override;

private:
  /// The input bit a value is (an i1 input, or a 1-bit extract of an input).
  std::optional<InputBit> inputBit(Value v);
  /// The literal of a value, looking through single-input inverters (added
  /// to `path` if given).
  static Lit lit(Value v, bool neg = false,
                 SmallVectorImpl<Operation *> *path = nullptr);
};
} // namespace

std::optional<InputBit> SubstituteAssumedInputsPass::inputBit(Value v) {
  if (auto arg = dyn_cast<BlockArgument>(v))
    if (arg.getOwner() == getOperation().getBodyBlock() &&
        arg.getType().isInteger(1))
      return InputBit{arg, 0};
  if (auto ext = v.getDefiningOp<comb::ExtractOp>())
    if (auto arg = dyn_cast<BlockArgument>(ext.getInput()))
      if (arg.getOwner() == getOperation().getBodyBlock() &&
          ext.getType().isInteger(1))
        return InputBit{arg, ext.getLowBit()};
  return std::nullopt;
}

Lit SubstituteAssumedInputsPass::lit(Value v, bool neg,
                                     SmallVectorImpl<Operation *> *path) {
  while (auto inv = v.getDefiningOp<synth::aig::AndInverterOp>()) {
    if (inv.getInputs().size() != 1)
      break;
    if (path)
      path->push_back(inv);
    neg ^= inv.getInverted()[0];
    v = inv.getInputs()[0];
  }
  return {v, neg};
}

void SubstituteAssumedInputsPass::runOnOperation() {
  auto module = getOperation();
  Block *body = module.getBodyBlock();

  // What the assumptions say about input bits: a constant value, or
  // implications x -> h (from !(x & !h)) and h -> x (from !(!x & h)).
  /// A fact with the ops of its assumption that read the input bit; they
  /// keep reading it if the fact is used.
  struct Fact {
    Lit h;
    SmallVector<Operation *> ops;
  };
  struct Facts {
    std::optional<bool> constant;
    SmallVector<Operation *> constantOps;
    SmallVector<Fact> implies, impliedBy;
  };
  llvm::MapVector<InputBit, Facts> facts;
  for (auto op : body->getOps<verif::AssumeOp>()) {
    if (!isPlainAssume(op, body))
      continue;
    SmallVector<Operation *> path{op};
    Lit p = lit(op.getProperty(), false, &path);
    // assume(x) / assume(!x)
    if (auto bit = inputBit(p.value)) {
      auto &f = facts[*bit];
      if (!f.constant) {
        f.constant = !p.neg;
        f.constantOps = path;
      }
      continue;
    }
    // assume(!(a & b)) with an input bit among a, b; or assume(x == f).
    auto andOp = p.value.getDefiningOp<synth::aig::AndInverterOp>();
    if (andOp && p.neg && andOp.getInputs().size() == 2) {
      SmallVector<Operation *> pathA{andOp}, pathB{andOp};
      Lit a = lit(andOp.getInputs()[0], andOp.getInverted()[0], &pathA);
      Lit b = lit(andOp.getInputs()[1], andOp.getInverted()[1], &pathB);
      for (auto [x, other, xPath] :
           {std::tuple(a, b, &pathA), std::tuple(b, a, &pathB)}) {
        auto bit = inputBit(x.value);
        if (!bit)
          continue;
        auto &f = facts[*bit];
        // !(x & o): x -> !o. !(!x & o): o -> x.
        if (!x.neg)
          f.implies.push_back({!other, *xPath});
        else
          f.impliedBy.push_back({other, *xPath});
      }
      continue;
    }
    if (auto cmp = p.value.getDefiningOp<comb::ICmpOp>();
        cmp && cmp.getLhs().getType().isInteger(1) &&
        (cmp.getPredicate() == comb::ICmpPredicate::eq ||
         cmp.getPredicate() == comb::ICmpPredicate::ne)) {
      bool ne = (cmp.getPredicate() == comb::ICmpPredicate::ne) ^ p.neg;
      for (auto [x, other] : {std::pair(cmp.getLhs(), cmp.getRhs()),
                              std::pair(cmp.getRhs(), cmp.getLhs())}) {
        auto bit = inputBit(x);
        if (!bit)
          continue;
        auto &f = facts[*bit];
        Lit o = lit(other, ne);
        f.implies.push_back({o, {cmp}});
        f.impliedBy.push_back({o, {cmp}});
        break;
      }
    }
  }

  // The values of each input bit in the module (the argument, or extracts).
  DenseMap<InputBit, SmallVector<Value>> uses;
  for (auto arg : body->getArguments())
    if (arg.getType().isInteger(1))
      uses[{arg, 0}].push_back(arg);
  module.walk([&](comb::ExtractOp ext) {
    if (auto bit = inputBit(ext.getResult()))
      uses[*bit].push_back(ext.getResult());
  });

  // Does the combinational cone of `root` contain one of `targets`?
  auto reaches = [&](Value root, ArrayRef<Value> targets) {
    DenseSet<Value> targetSet(targets.begin(), targets.end());
    SmallVector<Value> stack{root};
    DenseSet<Value> seen;
    while (!stack.empty()) {
      Value v = stack.pop_back_val();
      if (!seen.insert(v).second)
        continue;
      if (targetSet.contains(v))
        return true;
      Operation *op = v.getDefiningOp();
      if (!op || isa<seq::CompRegOp, seq::FirRegOp, seq::InitialOp>(op))
        continue;
      for (auto operand : op->getOperands())
        stack.push_back(operand);
    }
    return false;
  };

  OpBuilder builder(module.getContext());
  bool changed = false;
  for (auto &[bit, f] : facts) {
    auto it = uses.find(bit);
    if (it == uses.end())
      continue;
    ArrayRef<Value> values = it->second;
    Value replacement;
    SmallPtrSet<Operation *, 8> keep;
    builder.setInsertionPoint(body->getTerminator());
    auto loc = bit.first.getLoc();
    if (f.constant) {
      replacement = hw::ConstantOp::create(
          builder, loc,
          builder.getIntegerAttr(builder.getI1Type(), *f.constant));
      keep.insert(f.constantOps.begin(), f.constantOps.end());
      ++numConstant;
    } else {
      // x == h: an implication each way with the same h.
      for (auto &a : f.implies) {
        for (auto &b : f.impliedBy) {
          if (!(a.h == b.h) || reaches(a.h.value, values))
            continue;
          replacement = a.h.neg ? synth::aig::AndInverterOp::create(
                                      builder, loc, a.h.value, true)
                                : a.h.value;
          keep.insert(a.ops.begin(), a.ops.end());
          keep.insert(b.ops.begin(), b.ops.end());
          break;
        }
        if (replacement)
          break;
      }
      if (!replacement)
        continue;
      ++numEqual;
    }
    // The assumptions that determine the bit keep reading it: they still
    // hold, so the substitution is exact in every trace, and a trace of the
    // result is one of the module.
    for (Value v : values)
      v.replaceUsesWithIf(replacement, [&](OpOperand &use) {
        return !keep.contains(use.getOwner());
      });
    changed = true;
  }
  if (!changed)
    markAllAnalysesPreserved();
}
