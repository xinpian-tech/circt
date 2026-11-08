//===- HWConeOfInfluence.cpp - Keep only logic that reaches outputs -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Remove the operations of a module that cannot affect its outputs. Unlike
// canonicalization this also removes cycles (registers feeding each other)
// and signals that are written but never read on a path to an output. Used to
// cut a model checking problem down to the cone of influence of its property
// output before export.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/HW/HWPasses.h"

namespace circt {
namespace hw {
#define GEN_PASS_DEF_HWCONEOFINFLUENCE
#include "circt/Dialect/HW/Passes.h.inc"
} // namespace hw
} // namespace circt

using namespace mlir;
using namespace circt;
using namespace circt::hw;

namespace {
struct HWConeOfInfluencePass
    : public hw::impl::HWConeOfInfluenceBase<HWConeOfInfluencePass> {
  void runOnOperation() override;
};
} // namespace

/// LLHD signals are read and written through their references; the writes
/// (`llhd.drv`) are kept for every live signal. The dialect is identified by
/// name to avoid a dependency of HW on LLHD.
static bool isSignalOp(Operation *op) {
  return op->getDialect() && op->getDialect()->getNamespace() == "llhd";
}

void HWConeOfInfluencePass::runOnOperation() {
  Block *body = getOperation().getBodyBlock();
  DenseSet<Operation *> live;
  SmallVector<Operation *> worklist;
  auto markValue = [&](Value value) {
    if (auto *def = value.getDefiningOp()) {
      // Mark the top-level op of the module that defines the value.
      while (def->getBlock() != body)
        def = def->getParentOp();
      if (live.insert(def).second)
        worklist.push_back(def);
    }
  };

  // Roots: the outputs, and ops that are observable by themselves
  // (assertions, prints, SV procedural code). Registers and other ops without
  // declared memory effects are only kept when they reach a root.
  auto isObservable = [](Operation *op) {
    auto *dialect = op->getDialect();
    if (!dialect)
      return true;
    auto ns = dialect->getNamespace();
    return ns == "verif" || ns == "sim" || ns == "sv";
  };
  for (auto &op : *body)
    if (op.hasTrait<OpTrait::IsTerminator>() || isObservable(&op))
      if (live.insert(&op).second)
        worklist.push_back(&op);

  while (!worklist.empty()) {
    auto *op = worklist.pop_back_val();
    // Operands, including values from above used inside regions.
    op->walk([&](Operation *nested) {
      for (auto operand : nested->getOperands())
        markValue(operand);
    });
    // Writes to a live signal (and projections of it) affect its readers.
    if (isSignalOp(op))
      for (auto result : op->getResults()) {
        // Only signal references (not, e.g., shared time constants).
        if (result.getType().getAbstractType().getName() != "llhd.ref")
          continue;
        for (auto *user : result.getUsers()) {
          while (user->getBlock() != body)
            user = user->getParentOp();
          if (isSignalOp(user) && live.insert(user).second)
            worklist.push_back(user);
        }
      }
  }

  SmallVector<Operation *> dead;
  for (auto &op : *body)
    if (!live.contains(&op))
      dead.push_back(&op);
  for (auto *op : dead)
    op->dropAllReferences();
  for (auto *op : dead)
    op->erase();
  numErasedOps += dead.size();
}
