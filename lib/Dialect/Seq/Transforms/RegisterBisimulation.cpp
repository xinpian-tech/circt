//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the RegisterBisimulation pass: structural hashing
// through registers, on words. Values are partitioned by partition
// refinement, starting optimistically with all registers of the same type,
// clock and constant initial value in one class and all operations of the
// same kind and attributes in one class, and splitting classes whose members
// read operands of different classes, until nothing splits. Members of a
// class are then equal in every reachable state: they start equal (same
// initial value) and, if all classes hold in a cycle, compute the same
// function of the same values, so they hold in the next one. No SAT solver
// is involved; it finds the registers and logic that are duplicated
// structurally, such as the shadow registers of assertions that mirror the
// design, on whole words.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Seq/SeqPasses.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include <map>

namespace circt {
namespace seq {
#define GEN_PASS_DEF_REGISTERBISIMULATION
#include "circt/Dialect/Seq/SeqPasses.h.inc"
} // namespace seq
} // namespace circt

using namespace mlir;
using namespace circt;
using namespace seq;

namespace {
struct RegisterBisimulationPass
    : public seq::impl::RegisterBisimulationBase<RegisterBisimulationPass> {
  void runOnOperation() override;
};
} // namespace

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

/// The attributes that make two operations of the same name compute
/// different functions (names and hints do not).
static llvm::hash_code attributeHash(Operation *op) {
  llvm::hash_code hash = 0;
  for (auto attr : op->getAttrDictionary()) {
    StringRef name = attr.getName().getValue();
    if (name == "name" || name == "sv.namehint" || name == "sv.attributes" ||
        name == "twoState")
      continue;
    hash = llvm::hash_combine(hash, attr.getName(), attr.getValue());
  }
  return hash;
}

void RegisterBisimulationPass::runOnOperation() {
  Block *body = getOperation().getBodyBlock();

  // The values that can be merged: single results of side-effect free
  // operations without regions, and registers on which the propagation knows
  // all operands. Everything else is a class of its own.
  SmallVector<Operation *> ops;
  DenseMap<Value, unsigned> classOf;
  unsigned numClasses = 0;
  auto fresh = [&](Value v) { classOf[v] = numClasses++; };
  for (auto arg : body->getArguments())
    fresh(arg);

  // The kind of an operation: its name, result type and attributes, and for
  // a register its constant initial value. Operations of one kind start in
  // one class.
  DenseMap<Operation *, llvm::hash_code> kind;
  std::map<llvm::hash_code, unsigned> kindClass;
  for (auto &op : *body) {
    bool mergeable = false;
    llvm::hash_code k = llvm::hash_combine(op.getName().getAsOpaquePointer());
    if (auto reg = dyn_cast<CompRegOp>(op)) {
      mergeable = !reg.getInnerSymAttr();
      if (reg.getInitialValue()) {
        auto init = constantInitial(reg);
        mergeable &= init.has_value();
        if (init)
          k = llvm::hash_combine(k, llvm::hash_value(*init), true);
      }
    } else {
      mergeable = op.getNumResults() == 1 && op.getNumRegions() == 0 &&
                  isMemoryEffectFree(&op);
    }
    if (!mergeable) {
      for (auto result : op.getResults())
        fresh(result);
      continue;
    }
    k = llvm::hash_combine(k, op.getResult(0).getType().getAsOpaquePointer(),
                           attributeHash(&op));
    kind[&op] = k;
    auto [it, inserted] = kindClass.try_emplace(k, numClasses);
    if (inserted)
      ++numClasses;
    classOf[op.getResult(0)] = it->second;
    ops.push_back(&op);
  }
  // Values defined outside the module body (none in a module) and any
  // operand not seen yet get classes of their own.
  auto classOfOperand = [&](Value v) -> unsigned {
    auto it = classOf.find(v);
    if (it != classOf.end())
      return it->second;
    unsigned c = numClasses++;
    classOf[v] = c;
    return c;
  };

  // Refine: the new class of an operation is its old class and the classes
  // of its operands (sorted for commutative operations; for a register, not
  // its initial value, which is part of its kind). Repeat until the number
  // of classes no longer grows.
  unsigned rounds = 0;
  while (true) {
    ++rounds;
    std::map<SmallVector<unsigned>, unsigned> signatures;
    DenseMap<Value, unsigned> next;
    unsigned count = 0;
    for (auto *op : ops) {
      SmallVector<unsigned> sig{classOf.lookup(op->getResult(0))};
      SmallVector<unsigned> operands;
      if (auto reg = dyn_cast<CompRegOp>(op)) {
        operands.push_back(classOfOperand(reg.getInput()));
        operands.push_back(classOfOperand(reg.getClk()));
        operands.push_back(reg.getReset() ? classOfOperand(reg.getReset())
                                          : ~0u);
        operands.push_back(reg.getResetValue()
                               ? classOfOperand(reg.getResetValue())
                               : ~0u);
      } else {
        for (auto operand : op->getOperands())
          operands.push_back(classOfOperand(operand));
        if (op->hasTrait<OpTrait::IsCommutative>())
          llvm::sort(operands);
      }
      sig.append(operands);
      auto [it, inserted] = signatures.try_emplace(sig, count);
      if (inserted)
        ++count;
      next[op->getResult(0)] = it->second;
    }
    unsigned before = 0;
    {
      llvm::SmallDenseSet<unsigned> seen;
      for (auto *op : ops)
        seen.insert(classOf.lookup(op->getResult(0)));
      before = seen.size();
    }
    // Renumber the classes of the operations above those of the other values
    // so they do not collide.
    for (auto &[value, c] : next)
      classOf[value] = numClasses + c;
    numClasses += count;
    if (count == before)
      break;
  }
  numRounds += rounds;

  // Merge each class into its first member.
  DenseMap<unsigned, Value> representative;
  SmallVector<Operation *> merged;
  unsigned mergedRegs = 0, mergedOps = 0;
  for (auto *op : ops) {
    Value v = op->getResult(0);
    auto [it, inserted] = representative.try_emplace(classOf.lookup(v), v);
    if (inserted)
      continue;
    v.replaceAllUsesWith(it->second);
    merged.push_back(op);
    if (isa<CompRegOp>(op))
      ++mergedRegs;
    else
      ++mergedOps;
  }
  for (auto *op : merged)
    op->dropAllReferences();
  for (auto *op : merged)
    op->erase();
  numMergedRegisters += mergedRegs;
  numMergedOps += mergedOps;
  if (!mergedRegs && !mergedOps)
    markAllAnalysesPreserved();
}
