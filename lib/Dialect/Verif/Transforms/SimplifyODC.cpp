//===- SimplifyODC.cpp - Simplify properties under don't-cares -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Simplify the logic of assertions with observability don't-cares (ODCs): a
// value only has to be right where it is observed by the property, so facts
// that hold wherever it is observed can be substituted into it.
//
//   en: assert(p)         p is observed where en = 1
//   a |-> c               c is observed where a = 1
//   mux(s, x, y)          x is observed where s = 1, y where s = 0
//   a & b  /  a | b       b is observed where a = 1 / a = 0
//
// Facts are read from these conditions (conjuncts, negated disjuncts,
// `x == C`) and accumulate along the paths from the property, forming a
// context. Each value is rebuilt once per context in which it is reached, as a
// copy, so the design logic itself is left untouched. With only the first two
// rules (option `antecedent-only`), this substitutes the antecedent into the
// consequent:
//
//   a |-> c   ==   a |-> c[x := C for every fact x == C implied by a]
//
// The copy is then canonicalized and hashed with the rest of the cone, which
// folds the muxes that the facts decide; on datapath checks this often makes
// both sides of a comparison the same value, so that the consequent folds to
// true.
//
// If the consequent does not fold, the pass tries a case split on a 1-bit
// signal `s` that selects a mux in the cone: when `c[s := v]` folds to true,
//
//   a |-> c   ==   (a && s != v) |-> c[s := !v].
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LTL/LTLOps.h"
#include "circt/Dialect/Verif/VerifOps.h"
#include "circt/Dialect/Verif/VerifPasses.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include <map>
#include <unordered_map>

using namespace circt;
using namespace mlir;

namespace circt {
namespace verif {
#define GEN_PASS_DEF_SIMPLIFYODCPASS
#include "circt/Dialect/Verif/Passes.h.inc"
} // namespace verif
} // namespace circt

namespace {
struct SimplifyODCPass : verif::impl::SimplifyODCPassBase<SimplifyODCPass> {
  using SimplifyODCPassBase::SimplifyODCPassBase;
  LogicalResult initialize(MLIRContext *context) override;
  void runOnOperation() override;
  void simplify(ltl::ImplicationOp impl);
  bool split(ltl::ImplicationOp &impl);

  std::shared_ptr<const FrozenRewritePatternSet> patterns;
  /// One constant per value in the module, found once per run. These are
  /// never erased or rewritten, so that they stay valid merge targets.
  DenseMap<Attribute, Operation *> constants;
  DenseSet<Operation *> pinned;
};
} // namespace

/// Collect values that are constant whenever `cond` is `value`. Walks through
/// conjunctions, negated disjunctions and negations, and reads `x == C` as a
/// fact about `x`.
static void collectFacts(Value cond, bool value,
                         llvm::MapVector<Value, IntegerAttr> &facts) {
  SmallVector<std::pair<Value, bool>> worklist{{cond, value}};
  SmallPtrSet<Value, 16> seen;
  while (!worklist.empty()) {
    auto [value, polarity] = worklist.pop_back_val();
    if (!seen.insert(value).second)
      continue;
    auto *op = value.getDefiningOp();
    if (auto andOp = dyn_cast_or_null<comb::AndOp>(op); andOp && polarity) {
      for (auto input : andOp.getInputs())
        worklist.push_back({input, true});
      continue;
    }
    if (auto orOp = dyn_cast_or_null<comb::OrOp>(op); orOp && !polarity) {
      for (auto input : orOp.getInputs())
        worklist.push_back({input, false});
      continue;
    }
    if (auto xorOp = dyn_cast_or_null<comb::XorOp>(op);
        xorOp && xorOp.getNumOperands() == 2) {
      APInt c;
      if (matchPattern(xorOp.getInputs()[1], m_ConstantInt(&c))) {
        worklist.push_back({xorOp.getInputs()[0], polarity ^ c.isOne()});
        continue;
      }
    }
    if (matchPattern(value, m_Constant()))
      continue;
    facts.insert({value, IntegerAttr::get(value.getType(), polarity)});

    auto icmp = dyn_cast_or_null<comb::ICmpOp>(op);
    if (!icmp)
      continue;
    auto pred = icmp.getPredicate();
    bool isEq = pred == comb::ICmpPredicate::eq ||
                pred == comb::ICmpPredicate::ceq ||
                pred == comb::ICmpPredicate::weq;
    bool isNe = pred == comb::ICmpPredicate::ne ||
                pred == comb::ICmpPredicate::cne ||
                pred == comb::ICmpPredicate::wne;
    if (!((isEq && polarity) || (isNe && !polarity)))
      continue;
    APInt c;
    Value lhs = icmp.getLhs(), rhs = icmp.getRhs();
    if (matchPattern(rhs, m_ConstantInt(&c)) &&
        !matchPattern(lhs, m_Constant()) && isa<IntegerType>(lhs.getType()))
      facts.insert({lhs, IntegerAttr::get(lhs.getType(), c)});
    else if (matchPattern(lhs, m_ConstantInt(&c)) &&
             !matchPattern(rhs, m_Constant()) &&
             isa<IntegerType>(rhs.getType()))
      facts.insert({rhs, IntegerAttr::get(rhs.getType(), c)});
  }
}

/// Ops that may be copied into the consequent's cone: side-effect free ops of
/// the HW and Comb dialects without regions.
static bool isCopyable(Operation *op) {
  if (op->getNumRegions() != 0 || op->getNumResults() == 0)
    return false;
  if (!isa<comb::CombDialect, hw::HWDialect>(op->getDialect()))
    return false;
  if (isa<hw::InstanceOp>(op))
    return false;
  return isMemoryEffectFree(op);
}

/// Return `value` with the facts substituted, copying the ops between the
/// facts and `value` at the builder's insertion point. Returns `value` if
/// nothing changed.
static Value substitute(Value value,
                        const llvm::MapVector<Value, IntegerAttr> &facts,
                        OpBuilder &builder) {
  DenseMap<Value, Value> mapped;
  // Constants for the facts are created when first used.
  auto lookup = [&](Value v) -> Value {
    if (auto it = mapped.find(v); it != mapped.end())
      return it->second;
    auto it = facts.find(v);
    if (it == facts.end())
      return {};
    auto c = hw::ConstantOp::create(builder, v.getLoc(), it->second);
    return mapped[v] = c.getResult();
  };

  // Post-order walk over the cone; values not in `mapped` are unchanged.
  SmallVector<std::pair<Value, bool>> worklist{{value, false}};
  DenseSet<Value> visited;
  while (!worklist.empty()) {
    auto [v, expanded] = worklist.pop_back_val();
    if (mapped.count(v) || facts.count(v))
      continue;
    auto *op = v.getDefiningOp();
    if (!op || !isCopyable(op))
      continue;
    if (!expanded) {
      if (!visited.insert(v).second)
        continue;
      worklist.push_back({v, true});
      for (auto operand : op->getOperands())
        if (!visited.count(operand))
          worklist.push_back({operand, false});
      continue;
    }
    IRMapping mapping;
    for (auto operand : op->getOperands())
      if (auto to = lookup(operand))
        mapping.map(operand, to);
    if (mapping.getValueMap().empty())
      continue;
    auto *clone = builder.clone(*op, mapping);
    for (auto [from, to] : llvm::zip(op->getResults(), clone->getResults()))
      mapped[from] = to;
  }
  auto result = lookup(value);
  return result ? result : value;
}

namespace {
using Facts = llvm::MapVector<Value, IntegerAttr>;

/// Rebuilds a cone under contexts: sets of facts that hold wherever the value
/// being rebuilt is observed. Contexts are interned; each (value, context) is
/// rebuilt once.
class ODCRewriter {
public:
  ODCRewriter(OpBuilder &builder, bool nested, unsigned maxClones)
      : builder(builder), nested(nested), maxClones(maxClones) {}

  unsigned context(const Facts &facts) {
    SmallVector<std::pair<const void *, const void *>> key;
    for (auto [v, attr] : facts)
      key.push_back({v.getAsOpaquePointer(), attr.getAsOpaquePointer()});
    llvm::sort(key);
    auto [it, inserted] = ids.insert({key, contexts.size()});
    if (inserted)
      contexts.push_back(facts);
    return it->second;
  }

  /// The context `ctx` where `cond` is also `value`.
  unsigned extend(unsigned ctx, Value cond, bool value) {
    if (numClones >= maxClones)
      return ctx;
    Facts facts = contexts[ctx];
    auto size = facts.size();
    collectFacts(cond, value, facts);
    return facts.size() == size ? ctx : context(facts);
  }

  Value rewrite(Value value, unsigned ctx) { return visit(value, ctx, 0); }
  unsigned getNumClones() const { return numClones; }

private:
  Value constant(IntegerAttr attr, Location loc) {
    auto &c = constants[attr];
    if (!c)
      c = hw::ConstantOp::create(builder, loc, attr);
    return c;
  }

  Value visit(Value value, unsigned ctx, unsigned depth) {
    auto key = std::make_pair(value, ctx);
    if (auto it = memo.find(key); it != memo.end())
      return it->second;
    Value result = compute(value, ctx, depth);
    memo[key] = result;
    return result;
  }

  Value compute(Value value, unsigned ctx, unsigned depth) {
    if (auto it = contexts[ctx].find(value); it != contexts[ctx].end())
      return constant(it->second, value.getLoc());
    auto *op = value.getDefiningOp();
    if (!op || !isCopyable(op))
      return value;
    // Deep cones: substitute the context's facts without recursion (and
    // without opening new contexts), so that the stack stays bounded.
    if (depth >= 256)
      return substitute(value, contexts[ctx], builder);
    bool open = nested;
    ++depth;

    if (auto mux = dyn_cast<comb::MuxOp>(op)) {
      Value cond = visit(mux.getCond(), ctx, depth);
      APInt c;
      if (matchPattern(cond, m_ConstantInt(&c)))
        return visit(c.isOne() ? mux.getTrueValue() : mux.getFalseValue(), ctx,
                     depth);
      auto ctxTrue = open ? extend(ctx, mux.getCond(), true) : ctx;
      auto ctxFalse = open ? extend(ctx, mux.getCond(), false) : ctx;
      Value t = visit(mux.getTrueValue(), ctxTrue, depth);
      Value f = visit(mux.getFalseValue(), ctxFalse, depth);
      if (cond == mux.getCond() && t == mux.getTrueValue() &&
          f == mux.getFalseValue())
        return value;
      ++numClones;
      return comb::MuxOp::create(builder, mux.getLoc(), cond, t, f,
                                 mux.getTwoState());
    }

    // a & b: b only matters where a = 1 (and a | b: where a = 0). The
    // operands are taken in order, so that no operand is simplified with a
    // fact derived from itself.
    if (open && op->getResult(0).getType().isInteger(1) &&
        isa<comb::AndOp, comb::OrOp>(op)) {
      bool isAnd = isa<comb::AndOp>(op);
      SmallVector<Value> inputs;
      bool changed = false;
      unsigned cur = ctx;
      for (auto input : op->getOperands()) {
        Value v = visit(input, cur, depth);
        changed |= v != input;
        inputs.push_back(v);
        cur = extend(cur, input, isAnd);
      }
      if (!changed)
        return value;
      ++numClones;
      bool twoState = isAnd ? cast<comb::AndOp>(op).getTwoState()
                            : cast<comb::OrOp>(op).getTwoState();
      if (isAnd)
        return comb::AndOp::create(builder, op->getLoc(), inputs, twoState);
      return comb::OrOp::create(builder, op->getLoc(), inputs, twoState);
    }

    IRMapping mapping;
    for (auto operand : op->getOperands())
      if (auto v = visit(operand, ctx, depth); v != operand)
        mapping.map(operand, v);
    if (mapping.getValueMap().empty())
      return value;
    ++numClones;
    return builder.clone(*op, mapping)
        ->getResult(cast<OpResult>(value).getResultNumber());
  }

  OpBuilder &builder;
  bool nested;
  unsigned maxClones;
  unsigned numClones = 0;
  SmallVector<Facts> contexts;
  std::map<SmallVector<std::pair<const void *, const void *>>, unsigned> ids;
  DenseMap<std::pair<Value, unsigned>, Value> memo;
  DenseMap<Attribute, Value> constants;
};
} // namespace

/// The ops in the cone of `value` whose results are only used by `root` or by
/// other such ops: the logic that belongs to this assertion alone, which may
/// be rewritten without changing the design. Operands come before users. The
/// walk stops at shared logic, so its cost does not depend on the design.
static SmallVector<Operation *> ownedOps(Value value, Operation *root,
                                         const DenseSet<Operation *> &pinned) {
  DenseSet<Operation *> owned;
  SmallVector<Operation *> order, worklist;
  // Uses of each op's results by ops that are neither `root` nor owned. Each
  // use is counted once, so values with many users stay cheap.
  DenseMap<Operation *, unsigned> remaining;
  auto remainingUses = [&](Operation *op) -> unsigned & {
    auto [it, inserted] = remaining.try_emplace(op, 0);
    if (inserted)
      for (auto &use : op->getUses())
        if (use.getOwner() != root)
          ++it->second;
    return it->second;
  };
  auto ownable = [&](Operation *op) {
    return isCopyable(op) && !pinned.contains(op);
  };
  if (auto *op = value.getDefiningOp())
    if (ownable(op) && remainingUses(op) == 0)
      worklist.push_back(op);
  while (!worklist.empty()) {
    auto *op = worklist.pop_back_val();
    if (!owned.insert(op).second)
      continue;
    order.push_back(op);
    for (auto operand : op->getOperands()) {
      auto *def = operand.getDefiningOp();
      if (!def || owned.contains(def) || !ownable(def))
        continue;
      auto &count = remainingUses(def);
      if (--count == 0)
        worklist.push_back(def);
    }
  }
  std::reverse(order.begin(), order.end());
  return order;
}

static bool isTrue(Value value) {
  APInt c;
  return matchPattern(value, m_ConstantInt(&c)) && c.isOne();
}

/// Erase the ops of the cones of `values` that are left without uses.
static void eraseDead(ArrayRef<Value> values,
                      const DenseSet<Operation *> &pinned) {
  // Take the defining ops first: erasing one cone may free the others.
  SetVector<Operation *> worklist;
  for (auto value : values)
    if (auto *op = value.getDefiningOp())
      worklist.insert(op);
  DenseSet<Operation *> erased;
  while (!worklist.empty()) {
    auto *op = worklist.pop_back_val();
    if (erased.contains(op) || pinned.contains(op) || !op->use_empty() ||
        !(isCopyable(op) || isa<ltl::ImplicationOp>(op)))
      continue;
    for (auto operand : op->getOperands())
      if (auto *def = operand.getDefiningOp();
          def && def != op && !erased.contains(def))
        worklist.insert(def);
    erased.insert(op);
    op->erase();
  }
}

namespace {
/// `a + b == a + c` -> `b == c`, with an empty sum being 0. The consequent of
/// a datapath check often compares `x + acc` with `x` once the operand muxes
/// are folded.
struct CancelAddOperands : public OpRewritePattern<comb::ICmpOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(comb::ICmpOp op,
                                PatternRewriter &rewriter) const override {
    auto pred = op.getPredicate();
    if (pred != comb::ICmpPredicate::eq && pred != comb::ICmpPredicate::ne &&
        pred != comb::ICmpPredicate::ceq && pred != comb::ICmpPredicate::cne)
      return failure();
    auto terms = [](Value v) {
      SmallVector<Value> result;
      if (auto add = v.getDefiningOp<comb::AddOp>())
        result.append(add.getInputs().begin(), add.getInputs().end());
      else
        result.push_back(v);
      return result;
    };
    auto lhs = terms(op.getLhs()), rhs = terms(op.getRhs());
    bool changed = false;
    for (unsigned i = 0; i < lhs.size();) {
      auto *it = llvm::find(rhs, lhs[i]);
      if (it == rhs.end()) {
        ++i;
        continue;
      }
      rhs.erase(it);
      lhs.erase(lhs.begin() + i);
      changed = true;
    }
    if (!changed)
      return failure();
    auto type = op.getLhs().getType();
    auto sum = [&](ArrayRef<Value> values) -> Value {
      if (values.empty())
        return hw::ConstantOp::create(rewriter, op.getLoc(), type, 0);
      if (values.size() == 1)
        return values[0];
      return comb::AddOp::create(rewriter, op.getLoc(), values, false);
    };
    rewriter.replaceOpWithNewOp<comb::ICmpOp>(op, pred, sum(lhs), sum(rhs),
                                              op.getTwoState());
    return success();
  }
};
} // namespace

LogicalResult SimplifyODCPass::initialize(MLIRContext *context) {
  RewritePatternSet set(context);
  set.add<CancelAddOperands>(context);
  for (auto *dialect : context->getLoadedDialects())
    dialect->getCanonicalizationPatterns(set);
  for (auto op : context->getRegisteredOperations())
    op.getCanonicalizationPatterns(set, context);
  patterns = std::make_shared<FrozenRewritePatternSet>(std::move(set));
  return success();
}

/// Canonicalize the logic that belongs to the assertion only, and merge its
/// ops with structurally equal ones of the cone. Unlike CSE this ignores
/// discardable attributes: the two sides of a check usually carry different
/// `sv.namehint`s (`int_result` vs `mul_result`) and would not be merged.
void SimplifyODCPass::simplify(ltl::ImplicationOp impl) {
  Operation *root = impl;
  auto consequent = [&] { return impl.getConsequent(); };
  auto flags =
      OperationEquivalence::Flags(OperationEquivalence::IgnoreLocations |
                                  OperationEquivalence::IgnoreDiscardableAttrs);
  for (unsigned iter = 0; iter < 8; ++iter) {
    auto owned = ownedOps(consequent(), root, pinned);
    GreedyRewriteConfig config;
    config.setStrictness(GreedyRewriteStrictness::ExistingAndNewOps);
    bool changed = false;
    (void)applyOpPatternsGreedily(owned, *patterns, config, &changed);

    // Merge each owned op with an equivalent op that uses the same operands
    // (found through the operands' users, as CSE would), operands first.
    owned = ownedOps(consequent(), root, pinned);
    DenseMap<Value, Value> rep;
    auto repOf = [&](Value v) {
      auto it = rep.find(v);
      return it == rep.end() ? v : it->second;
    };
    auto equivalent = [&](Operation *op, Operation *other) {
      return other != op && other->getName() == op->getName() &&
             other->getNumResults() == op->getNumResults() &&
             !rep.count(other->getResult(0)) &&
             OperationEquivalence::isEquivalentTo(
                 op, other,
                 [&](Value a, Value b) {
                   return success(repOf(a) == repOf(b));
                 },
                 nullptr, flags);
    };
    std::unordered_multimap<size_t, Operation *> table;
    // Constants have no operands to find equals through: index the block's.
    for (auto *op : owned) {
      auto hash = size_t(OperationEquivalence::computeHash(
          op, [&](Value v) { return hash_value(repOf(v)); },
          OperationEquivalence::ignoreHashValue, flags));
      Operation *match = nullptr;
      // Owned ops seen before, then shared ops using the same first operand.
      auto range = table.equal_range(hash);
      for (auto it = range.first; it != range.second && !match; ++it)
        if (equivalent(op, it->second))
          match = it->second;
      if (auto c = dyn_cast<hw::ConstantOp>(op))
        if (auto *other = constants.lookup(c.getValueAttr());
            other && other != op)
          match = other;
      // Values with very many users are skipped: scanning them for every op
      // is quadratic, and only costs merges.
      Value first =
          op->getNumOperands() > 0 ? repOf(op->getOperand(0)) : Value();
      if (!match && first && !first.hasNUsesOrMore(257))
        for (auto *user : first.getUsers())
          if (user->getBlock() == op->getBlock() && equivalent(op, user)) {
            match = user;
            break;
          }
      if (!match) {
        table.insert({hash, op});
        continue;
      }
      for (auto [from, to] : llvm::zip(op->getResults(), match->getResults()))
        rep[from] = repOf(to);
    }
    // Only the assertion's own ops are rewired to the representatives.
    bool merged = false;
    SmallVector<Value> replaced;
    for (auto *op : owned)
      for (auto &operand : op->getOpOperands())
        if (auto r = repOf(operand.get()); r != operand.get()) {
          replaced.push_back(operand.get());
          operand.set(r);
          merged = true;
        }
    if (auto r = repOf(consequent()); r != consequent()) {
      replaced.push_back(consequent());
      impl->setOperand(1, r);
      merged = true;
    }
    eraseDead(replaced, pinned);
    if (!changed && !merged)
      break;
  }
}

/// 1-bit signals in the cones of the mux conditions of `value`'s cone that
/// are not themselves logic: inputs, registers, fields and bits of them.
static SmallVector<Value> splitCandidates(Value value, Operation *root,
                                          const DenseSet<Operation *> &pinned) {
  SetVector<Value> result;
  for (auto *op : ownedOps(value, root, pinned)) {
    auto mux = dyn_cast<comb::MuxOp>(op);
    if (!mux)
      continue;
    SmallVector<Value> worklist{mux.getCond()};
    DenseSet<Value> seen;
    while (!worklist.empty() && result.size() < 16) {
      auto v = worklist.pop_back_val();
      if (!seen.insert(v).second)
        continue;
      auto *def = v.getDefiningOp();
      if (!def || !isCopyable(def) ||
          isa<hw::StructExtractOp, comb::ExtractOp>(def)) {
        if (v.getType().isInteger(1) && !matchPattern(v, m_Constant()))
          result.insert(v);
        continue;
      }
      worklist.append(def->operand_begin(), def->operand_end());
    }
  }
  return result.takeVector();
}

/// Try a case split of `impl`; replace it and return true on success.
bool SimplifyODCPass::split(ltl::ImplicationOp &impl) {
  Value consequent = impl.getConsequent();
  if (matchPattern(consequent, m_Constant()))
    return false;
  for (auto s : splitCandidates(consequent, impl, pinned)) {
    for (bool v : {true, false}) {
      OpBuilder builder(impl);
      Facts facts;
      facts.insert({s, IntegerAttr::get(s.getType(), v)});
      Value cofactor = substitute(consequent, facts, builder);
      if (cofactor == consequent)
        continue;
      auto trial = ltl::ImplicationOp::create(builder, impl.getLoc(),
                                              impl.getAntecedent(), cofactor);
      simplify(trial);
      bool proved = isTrue(trial.getConsequent());
      Value rest = trial.getConsequent();
      trial.erase();
      eraseDead(rest, pinned);
      if (!proved)
        continue;

      // a |-> c  ==  (a && s == !v) |-> c[s := !v]
      facts.clear();
      facts.insert({s, IntegerAttr::get(s.getType(), !v)});
      Value other = substitute(consequent, facts, builder);
      Value literal = s;
      if (v)
        literal = comb::XorOp::create(
            builder, impl.getLoc(), s,
            hw::ConstantOp::create(builder, impl.getLoc(), s.getType(), 1));
      Value antecedent = comb::AndOp::create(
          builder, impl.getLoc(), impl.getAntecedent(), literal, false);
      auto newImpl =
          ltl::ImplicationOp::create(builder, impl.getLoc(), antecedent, other);
      impl.getResult().replaceAllUsesWith(newImpl.getResult());
      impl.erase();
      eraseDead(consequent, pinned);
      impl = newImpl;
      simplify(impl);
      return true;
    }
  }
  return false;
}

void SimplifyODCPass::runOnOperation() {
  constants.clear();
  pinned.clear();
  for (auto &op : *getOperation().getBodyBlock())
    if (auto c = dyn_cast<hw::ConstantOp>(op))
      if (constants.try_emplace(c.getValueAttr(), &op).second)
        pinned.insert(&op);
  SmallVector<Operation *> asserts;
  // Only properties in the module body: copies are placed before the
  // property, which needs a graph region (no dominance between ops).
  for (auto &op : *getOperation().getBodyBlock())
    if (isa<verif::AssertOp, verif::AssumeOp, verif::ClockedAssertOp,
            verif::ClockedAssumeOp>(op))
      asserts.push_back(&op);

  for (auto *op : asserts) {
    Value property = op->getOperand(0);
    Value enable;
    if (auto a = dyn_cast<verif::AssertOp>(op))
      enable = a.getEnable();
    else if (auto a = dyn_cast<verif::AssumeOp>(op))
      enable = a.getEnable();
    else if (auto a = dyn_cast<verif::ClockedAssertOp>(op))
      enable = a.getEnable();
    else if (auto a = dyn_cast<verif::ClockedAssumeOp>(op))
      enable = a.getEnable();

    Value antecedent, consequent;
    auto impl = property.getDefiningOp<ltl::ImplicationOp>();
    if (impl && impl.getAntecedent().getType().isInteger(1) &&
        impl.getConsequent().getType().isInteger(1)) {
      antecedent = impl.getAntecedent();
      consequent = impl.getConsequent();
    } else if (property.getType().isInteger(1)) {
      consequent = property;
    } else {
      continue;
    }

    OpBuilder builder(op);
    ODCRewriter rewriter(builder, !antecedentOnly, maxClones);
    unsigned ctx = rewriter.context({});
    if (enable)
      ctx = rewriter.extend(ctx, enable, true);
    if (antecedent)
      ctx = rewriter.extend(ctx, antecedent, true);
    Value newConsequent = rewriter.rewrite(consequent, ctx);
    numClones += rewriter.getNumClones();
    if (newConsequent == consequent)
      continue;
    ++numRewritten;

    // Work on an implication of our own, used by this assertion only.
    if (!antecedent)
      antecedent =
          hw::ConstantOp::create(builder, op->getLoc(), builder.getI1Type(), 1);
    auto newImpl = ltl::ImplicationOp::create(builder, op->getLoc(), antecedent,
                                              newConsequent);
    op->setOperand(0, newImpl);
    if (impl && impl->use_empty())
      impl->erase();
    eraseDead(consequent, pinned);
    simplify(newImpl);
    for (unsigned n = 0; n < maxSplits && split(newImpl); ++n)
      ++numSplits;
    if (isTrue(newImpl.getConsequent()) &&
        isa<verif::AssertOp, verif::ClockedAssertOp>(op)) {
      ++numProved;
      auto remark = op->emitRemark("property holds under its don't-cares");
      if (auto label = op->getAttrOfType<StringAttr>("label"))
        remark << " (label '" << label.getValue() << "')";
    }
  }
}
