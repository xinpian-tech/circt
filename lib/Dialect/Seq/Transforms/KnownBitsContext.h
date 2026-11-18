//===- KnownBitsContext.h - Known bits under facts --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Known bits of combinational values evaluated under facts (values assumed
// to have some known bits), shared by ContextSimplify and KnownBitsInvariants.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_LIB_DIALECT_SEQ_TRANSFORMS_KNOWNBITSCONTEXT_H
#define CIRCT_LIB_DIALECT_SEQ_TRANSFORMS_KNOWNBITSCONTEXT_H

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/KnownBits.h"
#include <optional>

namespace circt {
namespace seq {
namespace detail {
using llvm::KnownBits;
using mlir::APInt;
using mlir::Operation;
using mlir::Value;

/// Known bits of the result of `op` from those of its operands (in
/// KnownBitsInvariants.cpp).
KnownBits knownBitsTransfer(Operation *op, llvm::ArrayRef<KnownBits> in,
                            unsigned width);

inline unsigned widthOf(Value v) {
  if (auto t = llvm::dyn_cast<IntegerType>(v.getType()))
    return t.getWidth();
  return 0;
}

inline bool isComb(Operation *op) {
  return op && (llvm::isa<comb::CombDialect>(op->getDialect()) ||
                llvm::isa<hw::ConstantOp>(op));
}

inline std::optional<APInt> constantOf(Value v) {
  if (auto c = v.getDefiningOp<hw::ConstantOp>())
    return c.getValue();
  return std::nullopt;
}

/// Facts (known bits of values) and evaluation under them.
struct Context {
  llvm::DenseMap<Value, KnownBits> facts;
  llvm::DenseMap<Value, KnownBits> memo;
  bool contradiction = false;
  unsigned budget;
  unsigned depth;
  /// Known bits of the values in all states, if given: used where the
  /// evaluation stops.
  const llvm::DenseMap<Value, KnownBits> *base = nullptr;

  Context(unsigned budget, unsigned depth,
          const llvm::DenseMap<Value, KnownBits> *base = nullptr)
      : budget(budget), depth(depth), base(base) {}

  KnownBits leaf(Value v) const {
    if (base) {
      auto it = base->find(v);
      if (it != base->end())
        return it->second;
    }
    return KnownBits(widthOf(v));
  }

  void addKnown(Value v, const KnownBits &k, unsigned d) {
    unsigned w = widthOf(v);
    if (!w || (k.Zero | k.One).isZero())
      return;
    auto [it, inserted] = facts.try_emplace(v, k);
    if (!inserted) {
      KnownBits u = it->second.unionWith(k);
      if (u.hasConflict()) {
        contradiction = true;
        return;
      }
      if (u == it->second)
        return;
      it->second = u;
    }
    if (!d)
      return;
    Operation *op = v.getDefiningOp();
    if (!isComb(op))
      return;
    if (auto concat = llvm::dyn_cast<comb::ConcatOp>(op)) {
      unsigned pos = 0;
      for (auto input : llvm::reverse(concat.getInputs())) {
        unsigned iw = widthOf(input);
        addKnown(input, k.extractBits(iw, pos), d - 1);
        pos += iw;
      }
      return;
    }
    if (auto e = llvm::dyn_cast<comb::ExtractOp>(op)) {
      KnownBits s(widthOf(e.getInput()));
      s.insertBits(k, e.getLowBit());
      addKnown(e.getInput(), s, d - 1);
      return;
    }
    if (w != 1 || !k.isConstant())
      return;
    bool b = k.getConstant().getBoolValue();
    addBool(op, b, d - 1);
  }

  void addBool(Operation *op, bool b, unsigned d) {
    if (llvm::isa<comb::AndOp>(op) && b) {
      for (auto operand : op->getOperands())
        addKnown(operand, KnownBits::makeConstant(APInt(1, 1)), d);
      return;
    }
    if (llvm::isa<comb::OrOp>(op) && !b) {
      for (auto operand : op->getOperands())
        addKnown(operand, KnownBits::makeConstant(APInt(1, 0)), d);
      return;
    }
    if (auto x = llvm::dyn_cast<comb::XorOp>(op)) {
      if (x.getNumOperands() == 2)
        if (auto c = constantOf(x.getOperand(1)))
          addKnown(x.getOperand(0),
                   KnownBits::makeConstant(APInt(1, b ^ c->getBoolValue())), d);
      return;
    }
    if (auto icmp = llvm::dyn_cast<comb::ICmpOp>(op)) {
      using P = comb::ICmpPredicate;
      auto pred = icmp.getPredicate();
      bool isEq = pred == P::eq || pred == P::ceq || pred == P::weq;
      bool isNe = pred == P::ne || pred == P::cne || pred == P::wne;
      if (!isEq && !isNe)
        return;
      Value x = icmp.getLhs();
      auto c = constantOf(icmp.getRhs());
      if (!c) {
        x = icmp.getRhs();
        c = constantOf(icmp.getLhs());
      }
      if (!c)
        return;
      bool equal = isEq ? b : !b;
      if (equal)
        addKnown(x, KnownBits::makeConstant(*c), d);
      else if (c->getBitWidth() == 1)
        addKnown(x, KnownBits::makeConstant(~*c), d);
    }
  }

  /// The value `v` is under the facts, following muxes with known
  /// conditions.
  Value resolve(Value v) {
    for (unsigned d = 0; d < 64; ++d) {
      auto mux = v.getDefiningOp<comb::MuxOp>();
      if (!mux)
        break;
      KnownBits c = eval(mux.getCond(), depth);
      if (!c.isConstant())
        break;
      v = c.getConstant().getBoolValue() ? mux.getTrueValue()
                                         : mux.getFalseValue();
    }
    return v;
  }

  KnownBits eval(Value v, unsigned d) {
    unsigned w = widthOf(v);
    auto fact = facts.find(v);
    auto finish = [&](KnownBits k) {
      if (fact != facts.end()) {
        KnownBits u = k.unionWith(fact->second);
        k = u.hasConflict() ? fact->second : u;
      }
      return k;
    };
    if (auto c = constantOf(v))
      return KnownBits::makeConstant(*c);
    auto it = memo.find(v);
    if (it != memo.end())
      return it->second;
    Operation *op = v.getDefiningOp();
    if (!d || !budget || !isComb(op) || op->getNumResults() != 1)
      return finish(leaf(v));
    --budget;
    llvm::SmallVector<KnownBits> in;
    for (auto operand : op->getOperands()) {
      if (!widthOf(operand))
        return finish(leaf(v));
      in.push_back(eval(operand, d - 1));
    }
    KnownBits k = knownBitsTransfer(op, in, w);
    if (k.hasConflict())
      k = KnownBits(w);
    if (base) {
      KnownBits u = k.unionWith(leaf(v));
      if (!u.hasConflict())
        k = u;
    }
    k = finish(k);
    memo[v] = k;
    return k;
  }
};
} // namespace detail
} // namespace seq
} // namespace circt

#endif // CIRCT_LIB_DIALECT_SEQ_TRANSFORMS_KNOWNBITSCONTEXT_H
