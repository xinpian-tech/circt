//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the MuxDontCare pass. A mux that feeds a shift often
// has arms with bits that the shift can never bring into the used part of
// its result while that arm is selected, because the select constrains the
// shift amount:
//
//   full = split ? {resp, held} : {resp, resp}   // split implies offset != 0
//   data = (full >> 8 * offset)[31:0]
//
// Here held[7:0] is never seen. Such bits are observability don't cares;
// no equivalence-based simplification removes them, as the circuit holds
// no node equal to what they could be replaced with. This pass enumerates
// the bits the select and the shift amounts depend on (everything that is
// not a simple comb operator is a free input), collects for each arm the
// bits that can reach a used bit of a shift result while the arm is
// selected, and replaces the other bits of the arm by zero. The rewrite is
// exact on every input, so it needs no reachability and keeps all
// counterexamples.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/Comb/CombPasses.h"
#include "circt/Dialect/HW/HWOps.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

using namespace mlir;
using namespace circt;
using namespace comb;

namespace circt {
namespace comb {
#define GEN_PASS_DEF_MUXDONTCARE
#include "circt/Dialect/Comb/Passes.h.inc"
} // namespace comb
} // namespace circt

namespace {

static unsigned widthOf(Value v) {
  return cast<IntegerType>(v.getType()).getWidth();
}

/// Ops the evaluator computes; any other value is a free input.
static bool isEvaluated(Operation *op) {
  return isa_and_nonnull<hw::ConstantOp, AndOp, OrOp, XorOp, ICmpOp, ConcatOp,
                         ExtractOp, ReplicateOp, MuxOp, AddOp, SubOp, ShlOp,
                         ShrUOp>(op);
}

/// The free bits a set of values depends on, and their evaluation under an
/// assignment of these bits.
class Support {
public:
  Support(unsigned maxBits) : maxBits(maxBits) {}

  /// Adds the free bits of `v`. Returns false when there are too many.
  bool add(Value v) {
    if (!isa<IntegerType>(v.getType()))
      return false;
    auto *op = v.getDefiningOp();
    if (!isEvaluated(op))
      return addLeaf(v, APInt::getAllOnes(widthOf(v)));
    if (!visited.insert(v).second)
      return true;
    if (++numVisited > maxVisited)
      return false;
    if (auto extract = dyn_cast<ExtractOp>(op)) {
      Value input = extract.getInput();
      if (!isEvaluated(input.getDefiningOp())) {
        APInt mask = APInt::getBitsSet(widthOf(input), extract.getLowBit(),
                                       extract.getLowBit() + widthOf(v));
        return addLeaf(input, mask);
      }
    }
    for (Value operand : op->getOperands())
      if (!add(operand))
        return false;
    return true;
  }

  unsigned numBits() const { return totalBits; }

  /// Sets the free bits from the low bits of `assignment`.
  void assign(uint64_t assignment) {
    values.clear();
    unsigned bit = 0;
    for (auto &leaf : leaves) {
      APInt value(leaf.mask.getBitWidth(), 0);
      for (unsigned i = 0, e = leaf.mask.getBitWidth(); i != e; ++i)
        if (leaf.mask[i] && ((assignment >> bit++) & 1))
          value.setBit(i);
      values[leaf.value] = value;
    }
  }

  APInt evaluate(Value v) {
    auto it = values.find(v);
    if (it != values.end())
      return it->second;
    APInt result = compute(v);
    values[v] = result;
    return result;
  }

private:
  bool addLeaf(Value v, const APInt &mask) {
    auto [it, inserted] = leafIndex.try_emplace(v, leaves.size());
    if (inserted) {
      leaves.push_back({v, APInt(mask.getBitWidth(), 0)});
    }
    auto &leaf = leaves[it->second];
    APInt merged = leaf.mask | mask;
    totalBits += merged.popcount() - leaf.mask.popcount();
    leaf.mask = merged;
    return totalBits <= maxBits;
  }

  APInt compute(Value v) {
    auto *op = v.getDefiningOp();
    unsigned width = widthOf(v);
    if (auto c = dyn_cast<hw::ConstantOp>(op))
      return c.getValue();
    if (isa<AndOp, OrOp, XorOp, AddOp>(op)) {
      APInt result = evaluate(op->getOperand(0));
      for (Value operand : op->getOperands().drop_front()) {
        APInt x = evaluate(operand);
        if (isa<AndOp>(op))
          result &= x;
        else if (isa<OrOp>(op))
          result |= x;
        else if (isa<XorOp>(op))
          result ^= x;
        else
          result += x;
      }
      return result;
    }
    if (auto sub = dyn_cast<SubOp>(op))
      return evaluate(sub.getLhs()) - evaluate(sub.getRhs());
    if (auto icmp = dyn_cast<ICmpOp>(op)) {
      APInt l = evaluate(icmp.getLhs()), r = evaluate(icmp.getRhs());
      bool b;
      switch (icmp.getPredicate()) {
      case ICmpPredicate::eq:
      case ICmpPredicate::ceq:
      case ICmpPredicate::weq:
        b = l == r;
        break;
      case ICmpPredicate::ne:
      case ICmpPredicate::cne:
      case ICmpPredicate::wne:
        b = l != r;
        break;
      case ICmpPredicate::slt:
        b = l.slt(r);
        break;
      case ICmpPredicate::sle:
        b = l.sle(r);
        break;
      case ICmpPredicate::sgt:
        b = l.sgt(r);
        break;
      case ICmpPredicate::sge:
        b = l.sge(r);
        break;
      case ICmpPredicate::ult:
        b = l.ult(r);
        break;
      case ICmpPredicate::ule:
        b = l.ule(r);
        break;
      case ICmpPredicate::ugt:
        b = l.ugt(r);
        break;
      case ICmpPredicate::uge:
        b = l.uge(r);
        break;
      }
      return APInt(1, b);
    }
    if (auto concat = dyn_cast<ConcatOp>(op)) {
      APInt result(width, 0);
      unsigned pos = width;
      for (Value operand : concat.getInputs()) {
        APInt x = evaluate(operand);
        pos -= x.getBitWidth();
        result.insertBits(x, pos);
      }
      return result;
    }
    if (auto extract = dyn_cast<ExtractOp>(op))
      return evaluate(extract.getInput()).extractBits(width,
                                                      extract.getLowBit());
    if (auto replicate = dyn_cast<ReplicateOp>(op)) {
      APInt x = evaluate(replicate.getInput());
      APInt result(width, 0);
      for (unsigned pos = 0; pos < width; pos += x.getBitWidth())
        result.insertBits(x, pos);
      return result;
    }
    if (auto mux = dyn_cast<MuxOp>(op))
      return evaluate(mux.getCond()).isOne() ? evaluate(mux.getTrueValue())
                                             : evaluate(mux.getFalseValue());
    if (auto shl = dyn_cast<ShlOp>(op)) {
      APInt x = evaluate(shl.getLhs()), k = evaluate(shl.getRhs());
      return k.uge(width) ? APInt(width, 0) : x.shl(k.getZExtValue());
    }
    auto shr = cast<ShrUOp>(op);
    APInt x = evaluate(shr.getLhs()), k = evaluate(shr.getRhs());
    return k.uge(width) ? APInt(width, 0) : x.lshr(k.getZExtValue());
  }

  struct Leaf {
    Value value;
    APInt mask;
  };
  unsigned maxBits;
  static constexpr unsigned maxVisited = 4096;
  unsigned numVisited = 0;
  unsigned totalBits = 0;
  SmallVector<Leaf> leaves;
  DenseMap<Value, unsigned> leafIndex;
  DenseSet<Value> visited;
  DenseMap<Value, APInt> values;
};

/// The bits of `v` that are used: all of them, unless every user extracts.
static APInt usedBits(Value v) {
  unsigned width = widthOf(v);
  APInt used(width, 0);
  for (auto *user : v.getUsers()) {
    auto extract = dyn_cast<ExtractOp>(user);
    if (!extract)
      return APInt::getAllOnes(width);
    used.setBits(extract.getLowBit(),
                 extract.getLowBit() + widthOf(extract.getResult()));
  }
  return used;
}

/// The bits of the shifted value that reach a bit of `used` in the result
/// of `shift` by `k`.
static APInt reaching(Operation *shift, const APInt &used, const APInt &k) {
  unsigned width = used.getBitWidth();
  APInt result(width, 0);
  if (isa<ShlOp>(shift)) {
    if (k.ult(width))
      result = used.lshr(k.getZExtValue());
    return result;
  }
  bool inRange = k.ult(width);
  unsigned amount = inRange ? k.getZExtValue() : width;
  if (inRange)
    result = used.shl(amount);
  // An arithmetic shift copies the sign bit into the result bits whose
  // source is beyond the top.
  if (isa<ShrSOp>(shift) && amount != 0 &&
      !used.lshr(width - amount).isZero())
    result.setBit(width - 1);
  return result;
}

/// The bits of `v` known to be zero: constants and constant parts of a
/// concatenation.
static APInt knownZero(Value v) {
  unsigned width = widthOf(v);
  if (auto c = v.getDefiningOp<hw::ConstantOp>())
    return ~c.getValue();
  APInt result(width, 0);
  if (auto concat = v.getDefiningOp<ConcatOp>()) {
    unsigned pos = width;
    for (Value operand : concat.getInputs()) {
      pos -= widthOf(operand);
      if (auto c = operand.getDefiningOp<hw::ConstantOp>())
        result.insertBits(~c.getValue(), pos);
    }
  }
  return result;
}

struct MuxDontCarePass
    : public comb::impl::MuxDontCareBase<MuxDontCarePass> {
  using MuxDontCareBase::MuxDontCareBase;
  void runOnOperation() override;
  bool simplify(MuxOp mux);
};

bool MuxDontCarePass::simplify(MuxOp mux) {
  Value result = mux.getResult();
  if (!isa<IntegerType>(result.getType()) || result.use_empty())
    return false;
  unsigned width = widthOf(result);
  // Bits used whatever the select is, and the shifts to enumerate.
  APInt always(width, 0);
  SmallVector<std::pair<Operation *, APInt>> shifts;
  for (auto &use : result.getUses()) {
    auto *user = use.getOwner();
    if (auto extract = dyn_cast<ExtractOp>(user)) {
      always.setBits(extract.getLowBit(),
                     extract.getLowBit() + widthOf(extract.getResult()));
      continue;
    }
    if (isa<ShlOp, ShrUOp, ShrSOp>(user) && use.getOperandNumber() == 0 &&
        user->getOperand(1) != result) {
      shifts.push_back({user, usedBits(user->getResult(0))});
      continue;
    }
    return false;
  }
  if (shifts.empty())
    return false;

  Support support(maxSupportBits);
  if (!support.add(mux.getCond()))
    return false;
  for (auto &[shift, used] : shifts)
    if (!support.add(shift->getOperand(1)))
      return false;

  APInt seen[2] = {always, always}; // [0] false arm, [1] true arm
  for (uint64_t a = 0, e = uint64_t(1) << support.numBits(); a != e; ++a) {
    support.assign(a);
    unsigned arm = support.evaluate(mux.getCond()).isOne();
    for (auto &[shift, used] : shifts)
      seen[arm] |= reaching(shift, used, support.evaluate(shift->getOperand(1)));
  }

  bool changed = false;
  OpBuilder builder(mux);
  for (unsigned arm = 0; arm != 2; ++arm) {
    OpOperand &operand = mux->getOpOperand(arm == 1 ? 1 : 2);
    Value value = operand.get();
    APInt unseen = ~seen[arm] & ~knownZero(value);
    if (unseen.isZero())
      continue;
    // Rebuild the arm from its seen bits and zeros, most significant first.
    SmallVector<Value> parts;
    for (int hi = width; hi > 0;) {
      bool keep = !unseen[hi - 1];
      int lo = hi - 1;
      while (lo > 0 && !unseen[lo - 1] == keep)
        --lo;
      unsigned len = hi - lo;
      if (keep)
        parts.push_back(builder.createOrFold<ExtractOp>(
            mux.getLoc(), builder.getIntegerType(len), value, lo));
      else
        parts.push_back(hw::ConstantOp::create(builder, mux.getLoc(),
                                               APInt(len, 0)));
      hi = lo;
    }
    Value replacement = parts.size() == 1
                            ? parts.front()
                            : builder.createOrFold<ConcatOp>(mux.getLoc(),
                                                             parts);
    operand.set(replacement);
    ++numArms;
    numBits += unseen.popcount();
    changed = true;
  }
  return changed;
}

void MuxDontCarePass::runOnOperation() {
  SmallVector<MuxOp> muxes;
  getOperation().walk([&](MuxOp mux) { muxes.push_back(mux); });
  bool changed = false;
  for (auto mux : muxes)
    changed |= simplify(mux);
  if (!changed)
    markAllAnalysesPreserved();
}

} // namespace
