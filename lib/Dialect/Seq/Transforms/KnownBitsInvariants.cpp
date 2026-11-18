//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the KnownBitsInvariants pass: bits of registers (and
// of the logic they feed) that are constant in every reachable state, found
// by abstract interpretation on words. Every value is abstracted by its known
// bits (llvm::KnownBits). Registers start at their constant initial values;
// the combinational logic is evaluated on the abstraction (inputs unknown),
// the next states are joined with the current abstraction of each register
// (bits known in both and equal stay known), and this is repeated until it no
// longer changes. The result over-approximates the reachable states, so the
// bits still known are constant in all of them: registers are rebuilt with
// those bits as constants, and combinational values that are wholly known are
// replaced by constants. No SAT solver is used.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Seq/SeqPasses.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Matchers.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/KnownBits.h"

namespace circt {
namespace seq {
#define GEN_PASS_DEF_KNOWNBITSINVARIANTS
#include "circt/Dialect/Seq/SeqPasses.h.inc"
} // namespace seq
} // namespace circt

using namespace mlir;
using namespace circt;
using namespace seq;
using llvm::KnownBits;

namespace {
struct KnownBitsInvariantsPass
    : public seq::impl::KnownBitsInvariantsBase<KnownBitsInvariantsPass> {
  using KnownBitsInvariantsBase::KnownBitsInvariantsBase;
  void runOnOperation() override;
};
} // namespace

static std::optional<APInt> constantInitial(CompRegOp reg) {
  auto initial = reg.getInitialValue();
  if (!initial || !initial.getDefiningOp<InitialOp>())
    return std::nullopt;
  APInt value;
  if (!matchPattern(unwrapImmutableValue(initial), m_ConstantInt(&value)))
    return std::nullopt;
  return value;
}

static unsigned widthOf(Value v) {
  return cast<IntegerType>(v.getType()).getWidth();
}

/// Known bits of a shift: CIRCT gives 0 for amounts of at least the width.
template <typename F>
static KnownBits shift(const KnownBits &x, const KnownBits &amount, F f,
                       bool arithmetic) {
  unsigned w = x.getBitWidth();
  APInt widthValue(amount.getBitWidth(), w, /*isSigned=*/false,
                   /*implicitTrunc=*/true);
  bool widthFits = llvm::Log2_64_Ceil(w + 1) <= amount.getBitWidth();
  if (widthFits && amount.getMinValue().uge(widthValue)) {
    if (!arithmetic)
      return KnownBits::makeConstant(APInt(w, 0));
    // All copies of the sign bit.
    KnownBits r(w);
    if (x.isNegative())
      r.One.setAllBits();
    else if (x.isNonNegative())
      r.Zero.setAllBits();
    return r;
  }
  KnownBits r = f(x, amount);
  if (r.hasConflict())
    r = KnownBits(w);
  if (widthFits && amount.getMaxValue().uge(widthValue)) {
    KnownBits out(w);
    if (!arithmetic)
      out = KnownBits::makeConstant(APInt(w, 0));
    else if (x.isNegative())
      out.One.setAllBits();
    else if (x.isNonNegative())
      out.Zero.setAllBits();
    r = r.intersectWith(out);
  } else if (!widthFits) {
    // Amounts of any size: be conservative.
    r = r.intersectWith(KnownBits(w));
  }
  return r;
}

static KnownBits boolean(std::optional<bool> b) {
  KnownBits r(1);
  if (b) {
    if (*b)
      r.One.setAllBits();
    else
      r.Zero.setAllBits();
  }
  return r;
}

/// Known bits of the result of `op` from those of its operands; unknown for
/// operations it does not know.
static KnownBits transfer(Operation *op, ArrayRef<KnownBits> in,
                          unsigned width) {
  if (auto c = dyn_cast<hw::ConstantOp>(op))
    return KnownBits::makeConstant(c.getValue());
  if (isa<comb::AndOp>(op)) {
    KnownBits r = in[0];
    for (auto &k : in.drop_front())
      r &= k;
    return r;
  }
  if (isa<comb::OrOp>(op)) {
    KnownBits r = in[0];
    for (auto &k : in.drop_front())
      r |= k;
    return r;
  }
  if (isa<comb::XorOp>(op)) {
    KnownBits r = in[0];
    for (auto &k : in.drop_front())
      r ^= k;
    return r;
  }
  if (isa<comb::AddOp>(op)) {
    KnownBits r = in[0];
    for (auto &k : in.drop_front())
      r = KnownBits::add(r, k);
    return r;
  }
  if (isa<comb::SubOp>(op))
    return KnownBits::sub(in[0], in[1]);
  if (isa<comb::MulOp>(op)) {
    KnownBits r = in[0];
    for (auto &k : in.drop_front())
      r = KnownBits::mul(r, k);
    return r;
  }
  if (isa<comb::MuxOp>(op)) {
    if (in[0].isConstant())
      return in[0].getConstant().getBoolValue() ? in[1] : in[2];
    return in[1].intersectWith(in[2]);
  }
  if (auto extract = dyn_cast<comb::ExtractOp>(op))
    return in[0].extractBits(width, extract.getLowBit());
  if (isa<comb::ConcatOp>(op)) {
    KnownBits r = in.back();
    for (auto &k : llvm::reverse(in.drop_back()))
      r = k.concat(r);
    return r;
  }
  if (isa<comb::ReplicateOp>(op)) {
    KnownBits r(width);
    unsigned w = in[0].getBitWidth();
    for (unsigned pos = 0; pos < width; pos += w)
      r.insertBits(in[0], pos);
    return r;
  }
  if (isa<comb::ShlOp>(op))
    return shift(
        in[0], in[1],
        [](const KnownBits &a, const KnownBits &b) {
          return KnownBits::shl(a, b);
        },
        false);
  if (isa<comb::ShrUOp>(op))
    return shift(
        in[0], in[1],
        [](const KnownBits &a, const KnownBits &b) {
          return KnownBits::lshr(a, b);
        },
        false);
  if (isa<comb::ShrSOp>(op))
    return shift(
        in[0], in[1],
        [](const KnownBits &a, const KnownBits &b) {
          return KnownBits::ashr(a, b);
        },
        true);
  if (auto icmp = dyn_cast<comb::ICmpOp>(op)) {
    const KnownBits &a = in[0], &b = in[1];
    switch (icmp.getPredicate()) {
    case comb::ICmpPredicate::eq:
    case comb::ICmpPredicate::ceq:
    case comb::ICmpPredicate::weq:
      return boolean(KnownBits::eq(a, b));
    case comb::ICmpPredicate::ne:
    case comb::ICmpPredicate::cne:
    case comb::ICmpPredicate::wne:
      return boolean(KnownBits::ne(a, b));
    case comb::ICmpPredicate::ult:
      return boolean(KnownBits::ult(a, b));
    case comb::ICmpPredicate::ule:
      return boolean(KnownBits::ule(a, b));
    case comb::ICmpPredicate::ugt:
      return boolean(KnownBits::ugt(a, b));
    case comb::ICmpPredicate::uge:
      return boolean(KnownBits::uge(a, b));
    case comb::ICmpPredicate::slt:
      return boolean(KnownBits::slt(a, b));
    case comb::ICmpPredicate::sle:
      return boolean(KnownBits::sle(a, b));
    case comb::ICmpPredicate::sgt:
      return boolean(KnownBits::sgt(a, b));
    case comb::ICmpPredicate::sge:
      return boolean(KnownBits::sge(a, b));
    }
  }
  return KnownBits(width);
}

void KnownBitsInvariantsPass::runOnOperation() {
  Block *body = getOperation().getBodyBlock();

  // Registers analysed: no reset and a constant or no initial value (no
  // initial value: unknown at the start).
  SmallVector<CompRegOp> regs;
  for (auto reg : body->getOps<CompRegOp>()) {
    if (!isa<IntegerType>(reg.getType()) || reg.getReset() ||
        reg.getInnerSymAttr())
      continue;
    if (reg.getInitialValue() && !constantInitial(reg))
      continue;
    regs.push_back(reg);
  }
  DenseSet<Operation *> analysed;
  for (auto reg : regs)
    analysed.insert(reg);

  // Topological order of the combinational operations; registers, block
  // arguments and other operations are leaves. An operation closing a cycle
  // is unknown.
  SmallVector<Operation *> order;
  DenseSet<Operation *> cut;
  {
    DenseMap<Operation *, uint8_t> state;
    for (auto &root : *body) {
      if (!isa<comb::CombDialect>(root.getDialect()) &&
          !isa<hw::ConstantOp>(root))
        continue;
      if (state.lookup(&root))
        continue;
      SmallVector<std::pair<Operation *, unsigned>> stack{{&root, 0}};
      state[&root] = 1;
      while (!stack.empty()) {
        Operation *op = stack.back().first;
        unsigned k = stack.back().second++;
        if (k < op->getNumOperands()) {
          Operation *dep = op->getOperand(k).getDefiningOp();
          if (!dep || dep->getBlock() != body ||
              (!isa<comb::CombDialect>(dep->getDialect()) &&
               !isa<hw::ConstantOp>(dep)))
            continue;
          uint8_t s = state.lookup(dep);
          if (s == 1) {
            cut.insert(dep);
            continue;
          }
          if (s == 0) {
            state[dep] = 1;
            stack.push_back({dep, 0});
          }
          continue;
        }
        state[op] = 2;
        order.push_back(op);
        stack.pop_back();
      }
    }
  }

  DenseMap<Value, KnownBits> known;
  auto get = [&](Value v) -> KnownBits {
    auto it = known.find(v);
    if (it != known.end())
      return it->second;
    return KnownBits(widthOf(v));
  };
  // The abstraction of the registers, from their initial values.
  DenseMap<Operation *, KnownBits> state;
  for (auto reg : regs) {
    auto init = constantInitial(reg);
    state[reg] = init ? KnownBits::makeConstant(*init)
                      : KnownBits(widthOf(reg.getResult()));
  }
  unsigned rounds = 0;
  while (true) {
    ++rounds;
    known.clear();
    for (auto reg : regs)
      known[reg.getResult()] = state[reg];
    for (auto *op : order) {
      if (op->getNumResults() != 1 ||
          !isa<IntegerType>(op->getResultTypes()[0]))
        continue;
      unsigned w = widthOf(op->getResult(0));
      if (cut.contains(op)) {
        known[op->getResult(0)] = KnownBits(w);
        continue;
      }
      SmallVector<KnownBits> in;
      bool ints = true;
      for (auto operand : op->getOperands()) {
        if (!isa<IntegerType>(operand.getType())) {
          ints = false;
          break;
        }
        in.push_back(get(operand));
      }
      known[op->getResult(0)] = ints ? transfer(op, in, w) : KnownBits(w);
    }
    bool changed = false;
    for (auto reg : regs) {
      KnownBits next = get(reg.getInput()).intersectWith(state[reg]);
      if (next != state[reg]) {
        state[reg] = next;
        changed = true;
      }
    }
    if (!changed)
      break;
  }
  numRounds += rounds;

  // Rebuild: wholly known combinational values become constants, registers
  // with known bits read them as constants.
  OpBuilder builder(getOperation().getContext());
  unsigned constValues = 0, regBits = 0, constBits = 0;
  // A value with known bits read as a concatenation of its other bits and
  // the constants (all of it a constant if wholly known).
  auto rebuild = [&](Value v, const KnownBits &k, Operation *after) {
    APInt mask = k.Zero | k.One;
    if (mask.isZero() || v.use_empty())
      return;
    unsigned width = widthOf(v);
    Location loc = after->getLoc();
    builder.setInsertionPointAfter(after);
    bool isReg = isa<CompRegOp>(after);
    if (mask.isAllOnes()) {
      v.replaceAllUsesWith(hw::ConstantOp::create(builder, loc, k.getConstant()));
      if (isReg)
        regBits += width;
      else
        ++constValues;
      return;
    }
    if (!isReg && !partial)
      return;
    SmallVector<Value> parts;
    SmallPtrSet<Operation *, 8> created;
    for (int hi = width; hi > 0;) {
      bool isKnown = mask[hi - 1];
      int lo = hi - 1;
      while (lo > 0 && mask[lo - 1] == isKnown)
        --lo;
      unsigned len = hi - lo;
      if (isKnown) {
        parts.push_back(hw::ConstantOp::create(builder, loc,
                                               k.One.extractBits(len, lo)));
        if (isReg)
          regBits += len;
        else
          constBits += len;
      } else {
        auto e = comb::ExtractOp::create(builder, loc, v, lo, len);
        parts.push_back(e);
        created.insert(e);
      }
      hi = lo;
    }
    auto concat = comb::ConcatOp::create(builder, loc, parts);
    v.replaceAllUsesExcept(concat, created);
  };
  for (auto *op : order) {
    if (op->getNumResults() != 1 || isa<hw::ConstantOp>(op) ||
        cut.contains(op) || !isa<IntegerType>(op->getResultTypes()[0]))
      continue;
    // Not a bitwise rebuild of wiring (it would only add operations).
    if (isa<comb::ConcatOp, comb::ExtractOp, comb::ReplicateOp>(op) &&
        !known.lookup(op->getResult(0)).isConstant())
      continue;
    auto it = known.find(op->getResult(0));
    if (it != known.end())
      rebuild(op->getResult(0), it->second, op);
  }
  if (getenv("KNOWN_BITS_REPORT"))
    for (auto reg : regs) {
      const KnownBits &k = state[reg];
      llvm::errs() << "known-bits: " << reg.getName().value_or("?") << " "
                   << widthOf(reg.getResult()) << " "
                   << (k.Zero | k.One).popcount() << "\n";
    }
  for (auto reg : regs)
    rebuild(reg.getResult(), state[reg], reg);
  numConstantBits += constBits;
  numConstantValues += constValues;
  numRegisterBits += regBits;
  if (!constValues && !regBits)
    markAllAnalysesPreserved();
}
