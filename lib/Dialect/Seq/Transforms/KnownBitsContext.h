//===- KnownBitsContext.h - Known bits under facts --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Known bits of combinational values evaluated under facts (values assumed
// to have some known bits), shared by ContextSimplify, KnownBitsInvariants
// and UnshareOperators.
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

/// If `x` is `y + C`, `y - C`, `C - y` or `y ^ C`: `y`, and the value of `y`
/// for which `x` is `c` (these are bijections).
inline std::optional<std::pair<Value, APInt>> invertConstantOp(Value x,
                                                               const APInt &c) {
  Operation *op = x.getDefiningOp();
  if (!op || op->getNumOperands() != 2)
    return std::nullopt;
  Value lhs = op->getOperand(0), rhs = op->getOperand(1);
  auto cl = constantOf(lhs), cr = constantOf(rhs);
  if (llvm::isa<comb::AddOp>(op)) {
    if (cr)
      return std::make_pair(lhs, c - *cr);
    if (cl)
      return std::make_pair(rhs, c - *cl);
  } else if (llvm::isa<comb::SubOp>(op)) {
    if (cr)
      return std::make_pair(lhs, c + *cr);
    if (cl)
      return std::make_pair(rhs, *cl - c);
  } else if (llvm::isa<comb::XorOp>(op)) {
    if (cr)
      return std::make_pair(lhs, c ^ *cr);
    if (cl)
      return std::make_pair(rhs, c ^ *cl);
  }
  return std::nullopt;
}

/// Facts (known bits of values) and evaluation under them.
struct Context {
  llvm::DenseMap<Value, KnownBits> facts;
  /// Facts that are disequalities: constants a value is not.
  llvm::DenseMap<Value, llvm::SmallVector<APInt, 2>> excluded;
  llvm::DenseMap<Value, KnownBits> memo;
  bool contradiction = false;
  /// Facts that are disjunctions: an or known 1, an and known 0 (for unit
  /// propagation).
  llvm::SmallVector<std::pair<Operation *, bool>> clauses;
  unsigned budget;
  /// The budget of an evaluation.
  unsigned budgetLimit;
  unsigned depth;
  /// Known bits of the values in all states, if given: used where the
  /// evaluation stops.
  const llvm::DenseMap<Value, KnownBits> *base = nullptr;
  /// Remaining steps of an equality check, shared by its case splits.
  unsigned *steps = nullptr;

  Context(unsigned budget, unsigned depth,
          const llvm::DenseMap<Value, KnownBits> *base = nullptr)
      : budget(budget), budgetLimit(budget), depth(depth), base(base) {}

  /// A copy for a case split: without the memo (cleared by the new facts).
  Context branch() const {
    Context b(budgetLimit, depth, base);
    b.facts = facts;
    b.excluded = excluded;
    b.contradiction = contradiction;
    b.clauses = clauses;
    b.steps = steps;
    return b;
  }

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
    KnownBits now = facts.find(v)->second;
    if (now.isConstant()) {
      auto ex = excluded.find(v);
      if (ex != excluded.end() &&
          llvm::is_contained(ex->second, now.getConstant())) {
        contradiction = true;
        return;
      }
    }
    if (!d)
      return;
    Operation *op = v.getDefiningOp();
    if (!isComb(op))
      return;
    if (now.isConstant())
      if (auto inv = invertConstantOp(v, now.getConstant()))
        addKnown(inv->first, KnownBits::makeConstant(inv->second), d - 1);
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
      else
        addNotEqual(x, *c, d);
    }
  }

  /// Assume `x` is not `c`.
  void addNotEqual(Value x, const APInt &c, unsigned d) {
    if (c.getBitWidth() == 1)
      return addKnown(x, KnownBits::makeConstant(~c), d);
    if (knownNotEqual(x, c, 0))
      return;
    auto fact = facts.find(x);
    if (fact != facts.end() && fact->second.isConstant() &&
        fact->second.getConstant() == c) {
      contradiction = true;
      return;
    }
    excluded[x].push_back(c);
    memo.clear();
    if (!d)
      return;
    if (auto inv = invertConstantOp(x, c))
      addNotEqual(inv->first, inv->second, d - 1);
  }

  /// Whether `x` is not `c` under the facts: its known bits differ from `c`,
  /// or it is excluded (through bijections with constants).
  bool knownNotEqual(Value x, APInt c, unsigned d) {
    for (unsigned i = 0; i < 8; ++i) {
      auto ex = excluded.find(x);
      if (ex != excluded.end() && llvm::is_contained(ex->second, c))
        return true;
      KnownBits k = d ? eval(x, d) : facts.lookup(x);
      if (k.getBitWidth() == c.getBitWidth() &&
          !((k.One & ~c) | (k.Zero & c)).isZero())
        return true;
      auto inv = invertConstantOp(x, c);
      if (!inv)
        return false;
      std::tie(x, c) = *inv;
    }
    return false;
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

  /// A clause none of whose operands is known to satisfy it, with at least
  /// two open operands (at most `max`): the clause's value and the open
  /// operands.
  bool openClause(llvm::SmallVectorImpl<Value> &open, bool &value,
                  unsigned max) {
    for (auto [op, b] : clauses) {
      open.clear();
      bool satisfied = false;
      for (auto operand : op->getOperands()) {
        KnownBits k = eval(operand, depth);
        if (!k.isConstant())
          open.push_back(operand);
        else if (k.getConstant().getBoolValue() == b) {
          satisfied = true;
          break;
        }
      }
      if (!satisfied && open.size() >= 2 && open.size() <= max) {
        value = b;
        return true;
      }
    }
    open.clear();
    return false;
  }

  /// Calls `f` with the facts in each case of splits on the disjunctions
  /// among them (see openClause), up to `levels` deep and `extra` cases more
  /// than one in all (beyond that, with the facts unsplit); stops when `f`
  /// returns false. Does not call it in cases that are not possible.
  template <typename F>
  bool forEachCase(unsigned levels, unsigned &extra, F &&f) {
    llvm::SmallVector<Value> open;
    bool value;
    if (!levels || !extra || !openClause(open, value, extra + 1))
      return f(*this);
    extra -= open.size() - 1;
    for (unsigned i = 0; i < open.size(); ++i) {
      Context b = branch();
      bool possible = true;
      for (unsigned j = 0; j < i && possible; ++j)
        possible = b.assume(open[j], !value);
      if (!possible || !b.assume(open[i], value))
        continue;
      if (!b.forEachCase(levels - 1, extra, f))
        return false;
    }
    return true;
  }

  /// Assume `cond` is `b`; false if that contradicts the facts.
  bool assume(Value cond, bool b) {
    addKnown(cond, KnownBits::makeConstant(APInt(1, b)), depth);
    if (!contradiction)
      propagate();
    return !contradiction;
  }

  /// Whether `v` being `b` contradicts the facts: by unit propagation, or
  /// (up to `level` deep) because each operand of an or that would be 1 (an
  /// and that would be 0) being so does.
  bool refutes(Value v, bool b, unsigned level) {
    Context t = branch();
    if (!t.assume(v, b))
      return true;
    if (!level)
      return false;
    Operation *op = v.getDefiningOp();
    if (!op || !((llvm::isa<comb::OrOp>(op) && b) ||
                 (llvm::isa<comb::AndOp>(op) && !b)))
      return false;
    for (auto operand : op->getOperands()) {
      KnownBits k = t.eval(operand, depth);
      if (k.isConstant() && k.getConstant().getBoolValue() != b)
        continue;
      if (!t.refutes(operand, b, level - 1))
        return false;
    }
    return true;
  }

  /// The value of the i1 `c` under the facts, if known (with `level` deep
  /// refutation, see refutes).
  std::optional<bool> condition(Value c, unsigned level) {
    KnownBits k = eval(c, depth);
    if (k.isConstant())
      return k.getConstant().getBoolValue();
    if (!level || facts.empty())
      return std::nullopt;
    if (refutes(c, true, level))
      return false;
    if (refutes(c, false, level))
      return true;
    return std::nullopt;
  }

  /// The value `v` is under the facts, following muxes with known
  /// conditions (with `level` deep refutation, see refutes).
  Value resolve(Value v, unsigned level = 0) {
    for (unsigned d = 0; d < 64; ++d) {
      auto mux = v.getDefiningOp<comb::MuxOp>();
      if (!mux)
        break;
      auto c = condition(mux.getCond(), level);
      if (!c)
        break;
      v = *c ? mux.getTrueValue() : mux.getFalseValue();
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
    if (!excluded.empty() && w == 1 && !k.isConstant())
      if (auto icmp = llvm::dyn_cast<comb::ICmpOp>(op)) {
        using P = comb::ICmpPredicate;
        auto pred = icmp.getPredicate();
        bool isEq = pred == P::eq || pred == P::ceq || pred == P::weq;
        bool isNe = pred == P::ne || pred == P::cne || pred == P::wne;
        Value x = icmp.getLhs();
        auto c = constantOf(icmp.getRhs());
        if (!c) {
          x = icmp.getRhs();
          c = constantOf(icmp.getLhs());
        }
        if ((isEq || isNe) && c && knownNotEqual(x, *c, d - 1))
          k = KnownBits::makeConstant(APInt(1, isNe));
      }
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

/// A slice as slices of values that are not concatenations, extracts or
/// replications.
inline void normalizeSlice(Slice s, llvm::SmallVectorImpl<Slice> &out) {
  if (auto e = s.v.getDefiningOp<comb::ExtractOp>())
    return normalizeSlice({e.getInput(), s.lo + e.getLowBit(), s.w}, out);
  if (auto r = s.v.getDefiningOp<comb::ReplicateOp>()) {
    // Bit i is bit i mod iw of the input: the slice is pieces of copies.
    unsigned iw = widthOf(r.getInput());
    if (!iw)
      return out.push_back(s);
    for (unsigned pos = s.lo, end = s.lo + s.w; pos < end;) {
      unsigned lo = pos % iw, n = std::min(iw - lo, end - pos);
      normalizeSlice({r.getInput(), lo, n}, out);
      pos += n;
    }
    return;
  }
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

/// Known bits of a slice under the facts, slice by slice (through
/// concatenations, extracts and replications, and muxes whose condition is
/// known), so that the depth of the evaluation counts from the atoms only.
inline KnownBits knownSlice(Context &ctx, Slice s, unsigned level = 64) {
  llvm::SmallVector<Slice> atoms;
  normalizeSlice(s, atoms);
  KnownBits result(s.w);
  unsigned pos = 0;
  for (auto atom : atoms) {
    ctx.budget = ctx.budgetLimit;
    KnownBits k = ctx.eval(atom.v, ctx.depth).extractBits(atom.w, atom.lo);
    if (!k.isConstant() && level)
      if (auto mux = atom.v.getDefiningOp<comb::MuxOp>()) {
        ctx.budget = ctx.budgetLimit;
        KnownBits c = ctx.eval(mux.getCond(), ctx.depth);
        if (c.isConstant()) {
          Value arm = c.getConstant().getBoolValue() ? mux.getTrueValue()
                                                     : mux.getFalseValue();
          KnownBits ka = knownSlice(ctx, {arm, atom.lo, atom.w}, level - 1);
          KnownBits u = k.unionWith(ka);
          if (!u.hasConflict())
            k = u;
        }
      }
    result.insertBits(k, pos);
    pos += atom.w;
  }
  return result;
}

template <typename Same>
bool equalSlices(Context &ctx, Slice a, Slice b, unsigned splits, Same &same);

/// Two slices that are not concatenations or extracts.
template <typename Same>
bool equalAtoms(Context &c, Slice a, Slice b, unsigned splits, Same &same) {
  if (c.contradiction)
    return true;
  if (a.lo == b.lo && (a.v == b.v || same(a.v, b.v)))
    return true;
  c.budget = c.budgetLimit;
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
  if (c.steps) {
    if (!*c.steps)
      return false;
    --*c.steps;
  }
  // A disjunction (or a negated conjunction) in the facts: one case for each
  // operand that is the first to satisfy it.
  llvm::SmallVector<Value> open;
  bool value;
  if (c.openClause(open, value, 16)) {
    for (unsigned i = 0; i < open.size(); ++i) {
      Context branch = c.branch();
      bool possible = true;
      for (unsigned j = 0; j < i && possible; ++j)
        possible = branch.assume(open[j], !value);
      if (!possible || !branch.assume(open[i], value))
        continue;
      if (!equalSlices(branch, a, b, splits - 1, same))
        return false;
    }
    return true;
  }
  auto mux = a.v.getDefiningOp<comb::MuxOp>();
  bool onA = bool(mux);
  if (!mux)
    mux = b.v.getDefiningOp<comb::MuxOp>();
  if (!mux)
    return false;
  for (bool value : {true, false}) {
    Context branch = c.branch();
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
bool equalSlices(Context &ctx, Slice a, Slice b, unsigned splits, Same &same) {
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

/// Case splits of an equality check if not given.
constexpr unsigned defaultSteps = 256;

/// The bits where `a` and `b` are equal under the facts of `ctx`, by the
/// slices they are made of (see equalSlices).
template <typename Same>
APInt equalMask(const Context &facts, Value a, Value b, unsigned splits,
                Same same) {
  unsigned w = widthOf(a);
  if (a == b || same(a, b))
    return APInt::getAllOnes(w);
  Context ctx(facts);
  unsigned steps = defaultSteps;
  if (!ctx.steps)
    ctx.steps = &steps;
  // The aligned slices of `a` and `b`, at their position.
  struct Pair {
    Slice a, b;
    unsigned pos;
  };
  llvm::SmallVector<Pair> pairs;
  llvm::SmallVector<Slice> as, bs;
  normalizeSlice({a, 0, w}, as);
  normalizeSlice({b, 0, w}, bs);
  unsigned i = 0, j = 0, ia = 0, ib = 0, pos = 0;
  while (i < as.size() && j < bs.size()) {
    unsigned n = std::min(as[i].w - ia, bs[j].w - ib);
    pairs.push_back(
        {{as[i].v, as[i].lo + ia, n}, {bs[j].v, bs[j].lo + ib, n}, pos});
    pos += n;
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
  APInt mask(w, 0);
  for (auto &p : pairs)
    if (equalAtoms(ctx, p.a, p.b, splits, same))
      mask.setBits(p.pos, p.pos + p.a.w);
  return mask;
}

template <typename Same>
bool equalUnder(const Context &ctx, Value a, Value b, unsigned splits,
                Same same) {
  if (a == b || same(a, b))
    return true;
  Context c(ctx);
  unsigned steps = defaultSteps;
  if (!c.steps)
    c.steps = &steps;
  return equalSlices(c, {a, 0, widthOf(a)}, {b, 0, widthOf(b)}, splits, same);
}

} // namespace detail
} // namespace seq
} // namespace circt

#endif // CIRCT_LIB_DIALECT_SEQ_TRANSFORMS_KNOWNBITSCONTEXT_H
