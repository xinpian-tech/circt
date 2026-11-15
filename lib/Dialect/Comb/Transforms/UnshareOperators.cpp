//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the UnshareOperators pass. Synthesis shares an
// expensive operator between several uses by putting muxes on its operands;
// a property that states what the operator computes in one of those uses has
// its own copy of the operator. After bit-blasting, a solver has to prove the
// two copies equal gate by gate. This pass finds, for a multiplier (or a
// division, modulo or shift) whose operands go through muxes, the conditions on the mux selects under which
// its operands are those of another multiplier of the module, and takes the
// result from that other multiplier under these conditions:
//
//   m = mul(mux(s, a, c), mux(s, b, d));  e = mul(a, b)
//   ->  m' = mux(s, e, m)
//
// The rewrite is exact. It makes the module larger, and is only done when
// the other multiplier exists.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/Comb/CombPasses.h"
#include "circt/Dialect/HW/HWOps.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include <deque>

using namespace mlir;
using namespace circt;
using namespace comb;

namespace circt {
namespace comb {
#define GEN_PASS_DEF_UNSHAREOPERATORS
#include "circt/Dialect/Comb/Passes.h.inc"
} // namespace comb
} // namespace circt

namespace {

/// One bit of a value: bit `index` of `value`, or the constant `index` (0 or
/// 1) when `value` is null.
struct Bit {
  Value value;
  unsigned index;
  bool operator==(const Bit &other) const {
    return value == other.value && index == other.index;
  }
};
using Bits = SmallVector<Bit, 64>;

/// A select with its polarity removed: `xor(s, true)` is `s` inverted.
struct Select {
  Value base;
  bool inverted;
};

static Select normalizeSelect(Value select) {
  bool inverted = false;
  while (auto xorOp = select.getDefiningOp<XorOp>()) {
    if (xorOp.getNumOperands() != 2)
      break;
    auto constant = xorOp.getOperand(1).getDefiningOp<hw::ConstantOp>();
    if (!constant || !constant.getValue().isAllOnes())
      break;
    select = xorOp.getOperand(0);
    inverted = !inverted;
  }
  return {select, inverted};
}

/// A partial assignment of the selects.
using Assignment = SmallVector<std::pair<Value, bool>, 8>;

static std::optional<bool> lookup(const Assignment &assignment, Value base) {
  for (auto [value, isTrue] : assignment)
    if (value == base)
      return isTrue;
  return std::nullopt;
}

/// Append the bits [low, low + width) of `value` to `bits`, looking through
/// constants, concatenations, extracts, replications and the muxes whose
/// select is assigned. The first select found unassigned is stored in
/// `pending`; the bits of such a mux are bits of the mux itself.
static void collectBits(Value value, unsigned low, unsigned width,
                        const Assignment &assignment, Bits &bits,
                        Value &pending) {
  while (true) {
    auto *def = value.getDefiningOp();
    if (!def)
      break;
    if (auto constant = dyn_cast<hw::ConstantOp>(def)) {
      for (unsigned i = 0; i < width; ++i)
        bits.push_back({Value(), constant.getValue()[low + i]});
      return;
    }
    if (auto extract = dyn_cast<ExtractOp>(def)) {
      low += extract.getLowBit();
      value = extract.getInput();
      continue;
    }
    if (auto concat = dyn_cast<ConcatOp>(def)) {
      // Operands are listed from the most significant one.
      unsigned offset = 0;
      for (auto operand : llvm::reverse(concat.getInputs())) {
        unsigned operandWidth = hw::getBitWidth(operand.getType());
        unsigned begin = std::max(low, offset);
        unsigned end = std::min(low + width, offset + operandWidth);
        if (begin < end)
          collectBits(operand, begin - offset, end - begin, assignment, bits,
                      pending);
        offset += operandWidth;
      }
      return;
    }
    if (auto replicate = dyn_cast<ReplicateOp>(def)) {
      unsigned inputWidth = hw::getBitWidth(replicate.getInput().getType());
      for (unsigned i = 0; i < width; ++i)
        collectBits(replicate.getInput(), (low + i) % inputWidth, 1,
                    assignment, bits, pending);
      return;
    }
    if (auto mux = dyn_cast<MuxOp>(def)) {
      auto select = normalizeSelect(mux.getCond());
      if (auto isTrue = lookup(assignment, select.base)) {
        value = *isTrue != select.inverted ? mux.getTrueValue()
                                           : mux.getFalseValue();
        continue;
      }
      if (!pending)
        pending = select.base;
    }
    break;
  }
  for (unsigned i = 0; i < width; ++i)
    bits.push_back({value, low + i});
}

/// The value of `value` under an assignment of the selects: the muxes with
/// an assigned select are replaced by the chosen operand, through
/// concatenations, extracts and replications (rebuilt where they change).
static Value materialize(Value value, const Assignment &assignment,
                         OpBuilder &builder, DenseMap<Value, Value> &cache) {
  auto *def = value.getDefiningOp();
  if (!def)
    return value;
  auto it = cache.find(value);
  if (it != cache.end())
    return it->second;
  Value result = value;
  if (auto mux = dyn_cast<MuxOp>(def)) {
    auto select = normalizeSelect(mux.getCond());
    if (auto isTrue = lookup(assignment, select.base))
      result = materialize(*isTrue != select.inverted ? mux.getTrueValue()
                                                      : mux.getFalseValue(),
                           assignment, builder, cache);
  } else if (isa<ConcatOp, ExtractOp, ReplicateOp>(def)) {
    SmallVector<Value> operands;
    bool changed = false;
    for (auto operand : def->getOperands()) {
      operands.push_back(materialize(operand, assignment, builder, cache));
      changed |= operands.back() != operand;
    }
    if (changed) {
      auto *clone = def->clone();
      clone->setOperands(operands);
      builder.insert(clone);
      result = clone->getResult(0);
    }
  }
  cache[value] = result;
  return result;
}

struct UnshareOperatorsPass
    : public comb::impl::UnshareOperatorsBase<UnshareOperatorsPass> {
  using UnshareOperatorsBase::UnshareOperatorsBase;
  void runOnOperation() override;

private:
  /// The conditions under which the low bits of `shared` are those of
  /// another multiplier.
  struct Hit {
    Operation *other;
    SmallVector<Assignment, 2> assignments;
  };
  void findHits(Operation *shared, ArrayRef<Operation *> others,
                SmallVectorImpl<Hit> &hits);
  bool rewrite(Operation *shared, ArrayRef<Hit> hits);

  /// The (operator, other operator) pairs rewritten, to not also take the
  /// result of the second from the first.
  DenseSet<std::pair<Operation *, Operation *>> rewritten;
};

/// Whether `to` is reached from the result of `from` through ops of the
/// comb and hw dialects in the same block (registers, instances and anything
/// unknown end the search; a path through them is not combinational or is
/// not ours to judge, and then the rewrite is skipped by the caller only for
/// combinational paths).
static bool reaches(Operation *from, Operation *to) {
  DenseSet<Operation *> visited;
  SmallVector<Operation *> worklist{from};
  while (!worklist.empty()) {
    auto *op = worklist.pop_back_val();
    for (auto *user : op->getUsers()) {
      if (user == to)
        return true;
      if (user->getBlock() != from->getBlock())
        continue;
      if (!isa_and_nonnull<CombDialect, hw::HWDialect>(user->getDialect()) ||
          isa<hw::InstanceOp, hw::OutputOp>(user))
        continue;
      if (visited.insert(user).second)
        worklist.push_back(user);
    }
  }
  return false;
}

} // namespace

/// The operators handled: two-operand multiplications, divisions, modulos
/// and shifts.
static bool isCandidate(Operation *op) {
  return isa<MulOp, DivUOp, DivSOp, ModUOp, ModSOp, ShlOp, ShrUOp, ShrSOp>(
             op) &&
         op->getNumOperands() == 2;
}

static unsigned widthOf(Operation *op) {
  return hw::getBitWidth(op->getResult(0).getType());
}

void UnshareOperatorsPass::findHits(Operation *shared,
                                    ArrayRef<Operation *> others,
                                    SmallVectorImpl<Hit> &hits) {
  unsigned width = widthOf(shared);
  // Only for a multiplication the low bits of the result depend on the low
  // bits of the operands alone, and may the operands be swapped.
  bool isMul = isa<MulOp>(shared);
  std::deque<Assignment> queue;
  queue.push_back({});
  unsigned visited = 0;
  while (!queue.empty() && visited < maxPaths) {
    Assignment assignment = std::move(queue.front());
    queue.pop_front();
    ++visited;

    Value pending;
    Bits lhs, rhs;
    collectBits(shared->getOperand(0), 0, width, assignment, lhs, pending);
    collectBits(shared->getOperand(1), 0, width, assignment, rhs, pending);

    // Without an assignment the multiplier is not shared at all.
    bool matched = false;
    if (!assignment.empty()) {
      for (auto other : others) {
        if (other == shared || other->getName() != shared->getName())
          continue;
        unsigned low = std::min(width, widthOf(other));
        if (!isMul && widthOf(other) != width)
          continue;
        Value ignored;
        Bits otherLhs, otherRhs;
        collectBits(other->getOperand(0), 0, low, assignment, otherLhs,
                    ignored);
        collectBits(other->getOperand(1), 0, low, assignment, otherRhs,
                    ignored);
        auto equal = [&](const Bits &a, const Bits &b) {
          return std::equal(a.begin(), a.begin() + low, b.begin());
        };
        if (!((equal(lhs, otherLhs) && equal(rhs, otherRhs)) ||
              (isMul && equal(lhs, otherRhs) && equal(rhs, otherLhs))))
          continue;
        // Operands that became constants under the assignment (a register
        // in reset, say) match trivially and are not worth a condition.
        auto isConstant = [&](const Bits &bits) {
          return llvm::all_of(llvm::make_range(bits.begin(), bits.begin() + low),
                              [](const Bit &bit) { return !bit.value; });
        };
        if (isConstant(lhs) || isConstant(rhs))
          continue;
        auto *hit = llvm::find_if(
            hits, [&](const Hit &hit) { return hit.other == other; });
        if (hit == hits.end()) {
          hits.push_back({other, {}});
          hit = &hits.back();
        }
        hit->assignments.push_back(assignment);
        matched = true;
        break;
      }
    }
    if (matched || !pending)
      continue;
    for (bool isTrue : {true, false}) {
      Assignment next = assignment;
      next.push_back({pending, isTrue});
      queue.push_back(std::move(next));
    }
  }
}

bool UnshareOperatorsPass::rewrite(Operation *shared, ArrayRef<Hit> hits) {
  // Uses to redirect, collected before the new ops use the result.
  SmallVector<OpOperand *> uses;
  for (auto &use : shared->getResult(0).getUses())
    uses.push_back(&use);

  OpBuilder builder(shared->getContext());
  builder.setInsertionPointAfter(shared);
  auto loc = shared->getLoc();
  unsigned width = widthOf(shared);
  Value result = shared->getResult(0);
  bool changed = false;
  // Conditions under which the whole result comes from another operator.
  SmallVector<Value> unused;
  for (const Hit &hit : hits) {
    // Taking the result from a multiplier that depends on this one would
    // close a combinational cycle.
    if (reaches(shared, hit.other) || rewritten.contains({hit.other, shared}))
      continue;
    rewritten.insert({shared, hit.other});
    // The condition: one of the assignments holds.
    SmallVector<Value> terms;
    for (auto &assignment : hit.assignments) {
      SmallVector<Value> literals;
      for (auto [select, isTrue] : assignment)
        literals.push_back(isTrue ? select
                                  : comb::createOrFoldNot(builder, loc, select));
      terms.push_back(literals.size() == 1
                          ? literals.front()
                          : AndOp::create(builder, loc, literals, true));
    }
    unsigned otherWidth = widthOf(hit.other);
    unsigned low = std::min(width, otherWidth);
    Value otherLow = hit.other->getResult(0);
    if (otherWidth > low)
      otherLow = ExtractOp::create(builder, loc, otherLow, 0, low);
    if (low == width) {
      Value condition = terms.size() == 1
                            ? terms.front()
                            : OrOp::create(builder, loc, terms, true);
      result = MuxOp::create(builder, loc, condition, otherLow, result);
      unused.push_back(condition);
    } else if (widen) {
      // The other operator gives the low bits only. The high bits come from
      // a copy of the shared operator with the operands it has under the
      // assignment, so the whole result is replaced.
      for (auto [index, assignment] : llvm::enumerate(hit.assignments)) {
        DenseMap<Value, Value> cache;
        Value lhs =
            materialize(shared->getOperand(0), assignment, builder, cache);
        Value rhs =
            materialize(shared->getOperand(1), assignment, builder, cache);
        Value copy = MulOp::create(builder, loc, lhs, rhs);
        Value high = ExtractOp::create(builder, loc, copy, low, width - low);
        Value whole =
            ConcatOp::create(builder, loc, ValueRange{high, otherLow});
        result = MuxOp::create(builder, loc, terms[index], whole, result);
        unused.push_back(terms[index]);
        ++numWidened;
      }
    } else {
      Value condition = terms.size() == 1
                            ? terms.front()
                            : OrOp::create(builder, loc, terms, true);
      Value high = ExtractOp::create(builder, loc, result, low, width - low);
      Value oldLow = ExtractOp::create(builder, loc, result, 0, low);
      Value newLow = MuxOp::create(builder, loc, condition, otherLow, oldLow);
      result = ConcatOp::create(builder, loc, ValueRange{high, newLow});
    }
    changed = true;
    ++numConditions;
  }
  if (!changed)
    return false;
  for (auto *use : uses)
    use->set(result);
  // Under these conditions the result of the shared operator is not read
  // any more: give it constant operands there. Otherwise its result would
  // still equal the new one in every case, and an equivalence-merging pass
  // (fraig, signal correspondence) may put it back.
  if (isolate && !unused.empty()) {
    Value condition = unused.size() == 1
                          ? unused.front()
                          : OrOp::create(builder, loc, unused, true);
    for (auto &operand : shared->getOpOperands()) {
      // A zero divisor is avoided: one is as cheap.
      bool divisor = operand.getOperandNumber() == 1 &&
                     isa<DivUOp, DivSOp, ModUOp, ModSOp>(shared);
      Value constant = hw::ConstantOp::create(
          builder, loc, APInt(width, divisor ? 1 : 0));
      operand.set(
          MuxOp::create(builder, loc, condition, constant, operand.get()));
    }
    ++numIsolated;
  }
  return true;
}

void UnshareOperatorsPass::runOnOperation() {
  rewritten.clear();
  SmallVector<Operation *> candidates;
  for (auto &op : *getOperation().getBodyBlock())
    if (isCandidate(&op))
      candidates.push_back(&op);
  if (candidates.size() < 2)
    return markAllAnalysesPreserved();

  bool changed = false;
  for (auto *shared : candidates) {
    SmallVector<Hit, 1> hits;
    findHits(shared, candidates, hits);
    if (hits.empty())
      continue;
    if (rewrite(shared, hits)) {
      ++numUnshared;
      changed = true;
    }
  }
  if (!changed)
    markAllAnalysesPreserved();
}
