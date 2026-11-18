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
  /// Facts that are disjunctions: an or known 1, an and known 0 (for unit
  /// propagation).
  llvm::SmallVector<std::pair<Operation *, bool>> clauses;
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
    memo.clear();
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
    if ((llvm::isa<comb::OrOp>(op) && b) ||
        (llvm::isa<comb::AndOp>(op) && !b)) {
      clauses.push_back({op, b});
      return;
    }
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

  /// Unit propagation on the clauses: an or known 1 whose operands but one
  /// are known 0 has that one 1 (dually for and).
  void propagate() {
    for (unsigned round = 0; round <= clauses.size() && !contradiction;
         ++round) {
      bool changed = false;
      for (unsigned i = 0; i < clauses.size() && !contradiction; ++i) {
        auto [op, b] = clauses[i];
        Value open;
        unsigned numOpen = 0;
        bool satisfied = false;
        for (auto operand : op->getOperands()) {
          KnownBits k = eval(operand, depth);
          if (!k.isConstant()) {
            open = operand;
            ++numOpen;
          } else if (k.getConstant().getBoolValue() == b) {
            satisfied = true;
            break;
          }
        }
        if (satisfied)
          continue;
        if (numOpen == 0) {
          contradiction = true;
          break;
        }
        if (numOpen == 1) {
          addKnown(open, KnownBits::makeConstant(APInt(1, b)), depth);
          changed = true;
        }
      }
      if (!changed)
        break;
    }
  }

  /// Assume `cond` is `b`; false if that contradicts the facts.
  bool assume(Value cond, bool b) {
    addKnown(cond, KnownBits::makeConstant(APInt(1, b)), depth);
    if (!contradiction)
      propagate();
    return !contradiction;
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
/// A range of bits of a value.
struct Slice {
  Value v;
  unsigned lo, w;
};

/// A slice as slices of values that are not concatenations or extracts.
inline void normalizeSlice(Slice s, llvm::SmallVectorImpl<Slice> &out) {
  if (auto e = s.v.getDefiningOp<comb::ExtractOp>())
    return normalizeSlice({e.getInput(), s.lo + e.getLowBit(), s.w}, out);
  if (auto c = s.v.getDefiningOp<comb::ConcatOp>()) {
    unsigned pos = 0;
    for (auto input : llvm::reverse(c.getInputs())) {
      unsigned iw = widthOf(input);
      unsigned a = std::max(s.lo, pos), b = std::min(s.lo + s.w, pos + iw);
      if (a < b)
        normalizeSlice({input, a - pos, b - a}, out);
      pos += iw;
    }
    return;
  }
  out.push_back(s);
}

template <typename Same>
bool equalSlices(const Context &ctx, Slice a, Slice b, unsigned splits,
                 Same &same);

/// Two slices that are not concatenations or extracts.
template <typename Same>
bool equalAtoms(const Context &ctx, Slice a, Slice b, unsigned splits,
                Same &same) {
  if (ctx.contradiction)
    return true;
  if (a.lo == b.lo && (a.v == b.v || same(a.v, b.v)))
    return true;
  Context c(ctx);
  KnownBits ka = c.eval(a.v, c.depth).extractBits(a.w, a.lo);
  KnownBits kb = c.eval(b.v, c.depth).extractBits(b.w, b.lo);
  if (!((ka.Zero & kb.One) | (ka.One & kb.Zero)).isZero())
    return false;
  if (ka.isConstant() && kb.isConstant())
    return true;
  if (getenv("EQUAL_UNDER_TRACE")) {
    auto name = [](Value v) {
      if (auto *op = v.getDefiningOp())
        return op->getName().getStringRef();
      return llvm::StringRef("arg");
    };
    llvm::errs().indent(2 * (8 - splits))
        << name(a.v) << "[" << a.lo << "+" << a.w << "] " << name(b.v) << "["
        << b.lo << "+" << b.w << "]\n";
  }
  // Muxes with a known condition are their arm.
  for (Slice *s : {&a, &b}) {
    if (auto mux = s->v.getDefiningOp<comb::MuxOp>()) {
      KnownBits k = c.eval(mux.getCond(), c.depth);
      if (k.isConstant()) {
        s->v = k.getConstant().getBoolValue() ? mux.getTrueValue()
                                              : mux.getFalseValue();
        return equalSlices(c, a, b, splits, same);
      }
    }
  }
  if (!splits)
    return false;
  auto mux = a.v.getDefiningOp<comb::MuxOp>();
  bool onA = bool(mux);
  if (!mux)
    mux = b.v.getDefiningOp<comb::MuxOp>();
  if (!mux)
    return false;
  for (bool value : {true, false}) {
    Context branch(c);
    if (!branch.assume(mux.getCond(), value))
      continue;
    Value arm = value ? mux.getTrueValue() : mux.getFalseValue();
    Slice na = a, nb = b;
    (onA ? na : nb).v = arm;
    if (!equalSlices(branch, na, nb, splits - 1, same))
      return false;
  }
  return true;
}

/// Whether two slices are equal under the facts of `ctx`, splitting on the
/// conditions of muxes (up to `splits` deep). `same` says which values are
/// equal anyway.
template <typename Same>
bool equalSlices(const Context &ctx, Slice a, Slice b, unsigned splits,
                 Same &same) {
  llvm::SmallVector<Slice> as, bs;
  normalizeSlice(a, as);
  normalizeSlice(b, bs);
  // Align the boundaries.
  unsigned i = 0, j = 0, ia = 0, ib = 0; // bits consumed in as[i], bs[j]
  while (i < as.size() && j < bs.size()) {
    unsigned n = std::min(as[i].w - ia, bs[j].w - ib);
    if (!equalAtoms(ctx, {as[i].v, as[i].lo + ia, n},
                    {bs[j].v, bs[j].lo + ib, n}, splits, same))
      return false;
    ia += n;
    ib += n;
    if (ia == as[i].w) {
      ++i;
      ia = 0;
    }
    if (ib == bs[j].w) {
      ++j;
      ib = 0;
    }
  }
  return true;
}

template <typename Same>
bool equalUnder(const Context &ctx, Value a, Value b, unsigned splits,
                Same same) {
  if (a == b || same(a, b))
    return true;
  return equalSlices(ctx, {a, 0, widthOf(a)}, {b, 0, widthOf(b)}, splits, same);
}

} // namespace detail
} // namespace seq
} // namespace circt

#endif // CIRCT_LIB_DIALECT_SEQ_TRANSFORMS_KNOWNBITSCONTEXT_H
