//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the WordCongruence pass: registers and logic that are
// equal or constant in every reachable state, found on words without a
// solver. The abstraction is the reduced product of a partition of the
// registers (which are equal) and their known bits (which bits are constant):
//
//  - Registers start in classes of the same type, clock and constant initial
//    value, abstracted by the known bits of that value.
//  - Each round numbers the values of the logic, assuming the abstraction
//    holds: the registers of a class get one number (or a constant if all
//    their bits are known), operations are hash-consed on their operands'
//    numbers after rewriting (constant folding with known bits, flattening
//    and sorting of associative and commutative operations, concatenations
//    and extracts of the same word, muxes of equal arms, comparisons of equal
//    operands, bitwise operations made redundant by known bits).
//  - Classes whose members' next states get different numbers are split,
//    and the known bits of each class are joined with those of its members'
//    next states (which are equal if the class holds, so their known bits
//    are combined).
//
// The partition only gets finer and the known bits only coarser, so this
// stops. Its fixpoint holds initially and is preserved by a step, so it is an
// inductive invariant: the registers of a class are merged, values with one
// number are merged, and known bits become constants.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Seq/SeqPasses.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Matchers.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/KnownBits.h"
#include <map>
#include <unordered_map>

#include "KnownBitsContext.h"

namespace circt {
namespace seq {
#define GEN_PASS_DEF_WORDCONGRUENCE
#include "circt/Dialect/Seq/SeqPasses.h.inc"
namespace detail {
llvm::KnownBits knownBitsTransfer(Operation *op, ArrayRef<llvm::KnownBits> in,
                                  unsigned width);
} // namespace detail
} // namespace seq
} // namespace circt

using namespace mlir;
using namespace circt;
using namespace seq;
using llvm::KnownBits;

namespace {
struct WordCongruencePass
    : public seq::impl::WordCongruenceBase<WordCongruencePass> {
  using WordCongruenceBase::WordCongruenceBase;
  void runOnOperation() override;
  unsigned equalUnderMerges = 0;
};

/// Term codes: the operations the rewriting knows, then leaves, register
/// classes and constants. Other operations use their name's address.
enum Code : uint64_t {
  cAnd = 1,
  cOr,
  cXor,
  cAdd,
  cMul,
  cSub,
  cMux,
  cExtract,
  cConcat,
  cReplicate,
  cICmp,
  cShl,
  cShrU,
  cShrS,
  cLeaf,
  cReg,
  cConst,
  cLast
};

struct Term {
  uint64_t code;
  unsigned width;
  uint64_t attr; // extract: low bit; icmp: predicate; leaf, reg: id
  SmallVector<unsigned, 4> ops;
  APInt value; // constants
};

struct VecHash {
  size_t operator()(const std::vector<uint64_t> &v) const {
    return llvm::hash_combine_range(v.begin(), v.end());
  }
};

/// Hash-consed terms with rewriting.
struct Terms {
  std::vector<Term> terms;
  std::vector<std::optional<KnownBits>> known;
  std::unordered_map<std::vector<uint64_t>, unsigned, VecHash> table;

  void clear() {
    terms.clear();
    known.clear();
    table.clear();
  }
  unsigned width(unsigned t) const { return terms[t].width; }
  const Term &get(unsigned t) const { return terms[t]; }
  bool isConst(unsigned t) const { return terms[t].code == cConst; }

  unsigned intern(uint64_t code, unsigned width, uint64_t attr,
                  ArrayRef<unsigned> ops, const APInt *value = nullptr) {
    std::vector<uint64_t> key{code, width, attr, ops.size()};
    key.insert(key.end(), ops.begin(), ops.end());
    if (value)
      for (unsigned i = 0; i < value->getNumWords(); ++i)
        key.push_back(value->getRawData()[i]);
    auto [it, inserted] = table.try_emplace(std::move(key), terms.size());
    if (inserted) {
      terms.push_back({code, width, attr, SmallVector<unsigned, 4>(ops),
                       value ? *value : APInt()});
      known.push_back(std::nullopt);
      if (value)
        known.back() = KnownBits::makeConstant(*value);
    }
    return it->second;
  }

  KnownBits knownOf(unsigned t) const {
    if (known[t])
      return *known[t];
    return KnownBits(width(t));
  }
  void addKnown(unsigned t, const KnownBits &k) {
    if (!known[t]) {
      known[t] = k;
      return;
    }
    KnownBits u = known[t]->unionWith(k);
    if (!u.hasConflict())
      known[t] = u;
  }

  unsigned mkConst(const APInt &v) {
    return intern(cConst, v.getBitWidth(), 0, {}, &v);
  }

  unsigned mkExtract(unsigned x, unsigned lo, unsigned w) {
    unsigned xw = width(x);
    if (lo == 0 && w == xw)
      return x;
    Term t = get(x);
    switch (t.code) {
    case cConst:
      return mkConst(t.value.extractBits(w, lo));
    case cExtract:
      return mkExtract(t.ops[0], t.attr + lo, w);
    case cConcat: {
      SmallVector<unsigned> lsbFirst;
      unsigned pos = 0;
      for (unsigned piece : llvm::reverse(t.ops)) {
        unsigned pw = width(piece);
        unsigned a = std::max(lo, pos), b = std::min(lo + w, pos + pw);
        if (a < b)
          lsbFirst.push_back(mkExtract(piece, a - pos, b - a));
        pos += pw;
      }
      if (lsbFirst.size() == 1)
        return lsbFirst[0];
      SmallVector<unsigned> msbFirst(llvm::reverse(lsbFirst));
      return mkConcat(msbFirst);
    }
    case cReplicate: {
      unsigned iw = width(t.ops[0]);
      if (lo % iw + w <= iw)
        return mkExtract(t.ops[0], lo % iw, w);
      break;
    }
    default:
      break;
    }
    return intern(cExtract, w, lo, {x});
  }

  /// The word and low bit a term reads (itself at 0 if not an extract).
  std::pair<unsigned, unsigned> base(unsigned t) const {
    if (get(t).code == cExtract)
      return {get(t).ops[0], get(t).attr};
    return {t, 0};
  }

  unsigned mkConcat(ArrayRef<unsigned> msbFirst) {
    SmallVector<unsigned> flat;
    for (unsigned op : msbFirst) {
      if (get(op).code == cConcat)
        flat.append(get(op).ops.begin(), get(op).ops.end());
      else
        flat.push_back(op);
    }
    SmallVector<unsigned> result;
    for (unsigned p : flat) {
      if (!result.empty()) {
        unsigned q = result.back();
        if (isConst(q) && isConst(p)) {
          result.back() = mkConst(get(q).value.concat(get(p).value));
          continue;
        }
        auto [qb, qlo] = base(q);
        auto [pb, plo] = base(p);
        if (qb == pb && qlo == plo + width(p)) {
          result.back() = mkExtract(pb, plo, width(p) + width(q));
          continue;
        }
      }
      result.push_back(p);
    }
    if (result.size() == 1)
      return result[0];
    unsigned w = 0;
    for (unsigned p : result)
      w += width(p);
    return intern(cConcat, w, 0, result);
  }

  /// and, or, xor.
  unsigned mkBitwise(uint64_t code, unsigned w, ArrayRef<unsigned> in) {
    SmallVector<unsigned> flat;
    for (unsigned op : in) {
      if (get(op).code == code)
        flat.append(get(op).ops.begin(), get(op).ops.end());
      else
        flat.push_back(op);
    }
    APInt c = code == cAnd ? APInt::getAllOnes(w) : APInt(w, 0);
    SmallVector<unsigned> ops;
    std::map<unsigned, unsigned> count;
    for (unsigned op : flat) {
      if (isConst(op)) {
        const APInt &v = get(op).value;
        if (code == cAnd)
          c &= v;
        else if (code == cOr)
          c |= v;
        else
          c ^= v;
        continue;
      }
      ++count[op];
    }
    for (auto [op, n] : count)
      if (code != cXor || n % 2)
        ops.push_back(op);
    if (code == cAnd && c.isZero())
      return mkConst(c);
    if (code == cOr && c.isAllOnes())
      return mkConst(c);
    bool identity = code == cAnd ? c.isAllOnes() : c.isZero();
    if (!identity)
      ops.push_back(mkConst(c));
    // Drop an operand the others make redundant: for and, one that can be 0
    // only where another is known 0 (dually for or).
    if (code != cXor) {
      for (unsigned i = 0; i < ops.size();) {
        APInt others(w, 0);
        for (unsigned j = 0; j < ops.size(); ++j)
          if (j != i)
            others |= code == cAnd ? knownOf(ops[j]).Zero : knownOf(ops[j]).One;
        KnownBits k = knownOf(ops[i]);
        APInt mayDiffer = code == cAnd ? ~k.One : ~k.Zero;
        if (ops.size() > 1 && (mayDiffer & ~others).isZero()) {
          ops.erase(ops.begin() + i);
          continue;
        }
        ++i;
      }
    }
    llvm::sort(ops);
    if (ops.empty())
      return mkConst(c);
    if (ops.size() == 1)
      return ops[0];
    return intern(code, w, 0, ops);
  }

  unsigned mkAdd(unsigned w, ArrayRef<unsigned> in) {
    SmallVector<unsigned> ops;
    APInt c(w, 0);
    SmallVector<unsigned> flat;
    for (unsigned op : in) {
      if (get(op).code == cAdd)
        flat.append(get(op).ops.begin(), get(op).ops.end());
      else
        flat.push_back(op);
    }
    for (unsigned op : flat) {
      if (isConst(op))
        c += get(op).value;
      else
        ops.push_back(op);
    }
    if (!c.isZero())
      ops.push_back(mkConst(c));
    llvm::sort(ops);
    if (ops.empty())
      return mkConst(c);
    if (ops.size() == 1)
      return ops[0];
    return intern(cAdd, w, 0, ops);
  }

  unsigned mkMux(unsigned c, unsigned a, unsigned b, unsigned w) {
    if (a == b)
      return a;
    if (isConst(c))
      return get(c).value.getBoolValue() ? a : b;
    if (w == 1 && isConst(a) && isConst(b))
      return get(a).value.getBoolValue()
                 ? c
                 : mkBitwise(cXor, 1, {c, mkConst(APInt(1, 1))});
    return intern(cMux, w, 0, {c, a, b});
  }

  unsigned mkICmp(comb::ICmpPredicate pred, unsigned a, unsigned b) {
    using P = comb::ICmpPredicate;
    if (a == b) {
      switch (pred) {
      case P::eq:
      case P::ceq:
      case P::weq:
      case P::ule:
      case P::uge:
      case P::sle:
      case P::sge:
        return mkConst(APInt(1, 1));
      default:
        return mkConst(APInt(1, 0));
      }
    }
    bool symmetric = pred == P::eq || pred == P::ne || pred == P::ceq ||
                     pred == P::cne || pred == P::weq || pred == P::wne;
    if (symmetric && b < a)
      std::swap(a, b);
    return intern(cICmp, 1, static_cast<uint64_t>(pred), {a, b});
  }
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
  if (auto t = dyn_cast<IntegerType>(v.getType()))
    return t.getWidth();
  return 0;
}

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

/// The term of `op` from the terms of its operands, with result known `k`.
static unsigned number(Terms &t, Operation *op, ArrayRef<unsigned> in,
                       const KnownBits &k, unsigned w) {
  if (w && k.isConstant())
    return t.mkConst(k.getConstant());
  if (isa<comb::AndOp>(op))
    return t.mkBitwise(cAnd, w, in);
  if (isa<comb::OrOp>(op))
    return t.mkBitwise(cOr, w, in);
  if (isa<comb::XorOp>(op))
    return t.mkBitwise(cXor, w, in);
  if (isa<comb::AddOp>(op))
    return t.mkAdd(w, in);
  if (isa<comb::MulOp>(op)) {
    SmallVector<unsigned> ops(in);
    llvm::sort(ops);
    return t.intern(cMul, w, 0, ops);
  }
  if (isa<comb::SubOp>(op)) {
    if (in[0] == in[1])
      return t.mkConst(APInt(w, 0));
    if (t.isConst(in[1]) && t.get(in[1]).value.isZero())
      return in[0];
    return t.intern(cSub, w, 0, in);
  }
  if (isa<comb::MuxOp>(op))
    return t.mkMux(in[0], in[1], in[2], w);
  if (auto e = dyn_cast<comb::ExtractOp>(op))
    return t.mkExtract(in[0], e.getLowBit(), w);
  if (isa<comb::ConcatOp>(op))
    return t.mkConcat(in);
  if (isa<comb::ReplicateOp>(op)) {
    if (t.width(in[0]) == w)
      return in[0];
    return t.intern(cReplicate, w, 0, in);
  }
  if (auto icmp = dyn_cast<comb::ICmpOp>(op))
    return t.mkICmp(icmp.getPredicate(), in[0], in[1]);
  if (isa<comb::ShlOp, comb::ShrUOp, comb::ShrSOp>(op)) {
    if (t.isConst(in[1]) && t.get(in[1]).value.isZero())
      return in[0];
    uint64_t code = isa<comb::ShlOp>(op)    ? cShl
                    : isa<comb::ShrUOp>(op) ? cShrU
                                            : cShrS;
    return t.intern(code, w, 0, in);
  }
  SmallVector<unsigned> ops(in);
  if (op->hasTrait<OpTrait::IsCommutative>())
    llvm::sort(ops);
  return t.intern(
      reinterpret_cast<uint64_t>(op->getName().getAsOpaquePointer()) + cLast, w,
      attributeHash(op), ops);
}

void WordCongruencePass::runOnOperation() {
  Block *body = getOperation().getBodyBlock();

  // Registers in the partition: no reset, no inner symbol, a constant or no
  // initial value. The others are leaves.
  SmallVector<CompRegOp> regs;
  for (auto reg : body->getOps<CompRegOp>()) {
    if (!isa<IntegerType>(reg.getType()) || reg.getReset() ||
        reg.getInnerSymAttr())
      continue;
    if (reg.getInitialValue() && !constantInitial(reg))
      continue;
    regs.push_back(reg);
  }

  // Topological order of the combinational operations; an operation closing
  // a cycle is a leaf.
  SmallVector<Operation *> order;
  DenseSet<Operation *> cut;
  {
    DenseMap<Operation *, uint8_t> state;
    auto inOrder = [&](Operation *op) {
      return op && op->getBlock() == body &&
             (isa<comb::CombDialect>(op->getDialect()) ||
              isa<hw::ConstantOp>(op));
    };
    for (auto &root : *body) {
      if (!inOrder(&root) || state.lookup(&root))
        continue;
      SmallVector<std::pair<Operation *, unsigned>> stack{{&root, 0}};
      state[&root] = 1;
      while (!stack.empty()) {
        Operation *op = stack.back().first;
        unsigned k = stack.back().second++;
        if (k < op->getNumOperands()) {
          Operation *dep = op->getOperand(k).getDefiningOp();
          if (!inOrder(dep))
            continue;
          uint8_t s = state.lookup(dep);
          if (s == 1)
            cut.insert(dep);
          else if (s == 0) {
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

  // Initial classes: same width, clock and initial value (registers without
  // one get classes of their own).
  SmallVector<unsigned> cls(regs.size());
  SmallVector<KnownBits> known; // per class
  {
    std::map<std::tuple<unsigned, void *, std::string>, unsigned> keys;
    for (auto [i, reg] : llvm::enumerate(regs)) {
      auto init = constantInitial(reg);
      unsigned w = widthOf(reg.getResult());
      if (!init) {
        cls[i] = known.size();
        known.push_back(KnownBits(w));
        continue;
      }
      SmallString<32> str;
      init->toStringUnsigned(str, 16);
      auto key = std::make_tuple(w, reg.getClk().getAsOpaquePointer(),
                                 std::string(str));
      auto [it, inserted] = keys.try_emplace(key, known.size());
      if (inserted)
        known.push_back(KnownBits::makeConstant(*init));
      cls[i] = it->second;
    }
  }

  Terms terms;
  DenseMap<Value, unsigned> vn;
  DenseMap<Value, KnownBits> kb;
  DenseMap<Value, unsigned> leafIds;
  DenseMap<unsigned, Value> leafValue; // last round
  auto numberOf = [&](Value v) -> unsigned {
    auto it = vn.find(v);
    if (it != vn.end())
      return it->second;
    unsigned id = leafIds.try_emplace(v, leafIds.size()).first->second;
    unsigned t = terms.intern(cLeaf, widthOf(v), id, {});
    leafValue[t] = v;
    vn[v] = t;
    return t;
  };
  auto knownOf = [&](Value v) -> KnownBits {
    auto it = kb.find(v);
    if (it != kb.end())
      return it->second;
    return KnownBits(widthOf(v));
  };

  unsigned rounds = 0;
  while (true) {
    ++rounds;
    terms.clear();
    vn.clear();
    kb.clear();
    leafValue.clear();
    for (auto [i, reg] : llvm::enumerate(regs)) {
      Value v = reg.getResult();
      const KnownBits &k = known[cls[i]];
      unsigned t = k.isConstant() ? terms.mkConst(k.getConstant())
                                  : terms.intern(cReg, widthOf(v), cls[i], {});
      terms.addKnown(t, k);
      vn[v] = t;
      kb[v] = k;
    }
    for (auto *op : order) {
      if (op->getNumResults() != 1 || !widthOf(op->getResult(0)))
        continue;
      Value v = op->getResult(0);
      unsigned w = widthOf(v);
      if (cut.contains(op)) {
        numberOf(v);
        continue;
      }
      SmallVector<KnownBits> in;
      SmallVector<unsigned> ins;
      bool ints = true;
      for (auto operand : op->getOperands()) {
        ins.push_back(numberOf(operand));
        if (!widthOf(operand))
          ints = false;
        else
          in.push_back(knownOf(operand));
      }
      KnownBits k =
          ints ? seq::detail::knownBitsTransfer(op, in, w) : KnownBits(w);
      if (k.hasConflict())
        k = KnownBits(w);
      unsigned t = ints ? number(terms, op, ins, k, w)
                        : number(terms, op, ins, KnownBits(w), w);
      terms.addKnown(t, k);
      // The term may know more (another value of the same number).
      kb[v] = terms.knownOf(t);
      vn[v] = t;
    }

    // Split by next-state number.
    std::map<std::pair<unsigned, unsigned>, unsigned> keys;
    SmallVector<unsigned> raw(regs.size());
    SmallVector<unsigned> rawOld, rawFirst; // per raw class
    for (auto [i, reg] : llvm::enumerate(regs)) {
      unsigned t = numberOf(reg.getInput());
      auto [it, inserted] = keys.try_emplace({cls[i], t}, rawOld.size());
      if (inserted) {
        rawOld.push_back(cls[i]);
        rawFirst.push_back(i);
      }
      raw[i] = it->second;
    }
    // Next states with different numbers may still be equal under the
    // invariant: split on mux conditions to show it (a few candidates per
    // class).
    if (const char *pair = getenv("WC_PAIR")) {
      auto [x, y] = StringRef(pair).split(',');
      int ix = -1, iy = -1;
      for (auto [i, reg] : llvm::enumerate(regs)) {
        if (reg.getName() && *reg.getName() == x)
          ix = i;
        if (reg.getName() && *reg.getName() == y)
          iy = i;
      }
      if (ix >= 0 && iy >= 0) {
        llvm::errs() << "round " << rounds << " old " << cls[ix] << "/"
                     << cls[iy] << " raw " << raw[ix] << "/" << raw[iy] << "\n";
        if (cls[ix] == cls[iy] && raw[ix] != raw[iy]) {
          auto same = [&](Value a, Value b) {
            auto p = vn.find(a), q = vn.find(b);
            return p != vn.end() && q != vn.end() && p->second == q->second;
          };
          setenv("EQUAL_UNDER_TRACE", "1", 1);
          seq::detail::Context ctx(budget, depth, &kb);
          bool r = seq::detail::equalUnder(ctx, regs[ix].getInput(),
                                           regs[iy].getInput(), splits, same);
          unsetenv("EQUAL_UNDER_TRACE");
          llvm::errs() << " equalUnder " << r << "\n";
        }
      }
    }
    SmallVector<unsigned> canon(rawOld.size());
    for (unsigned r = 0; r < rawOld.size(); ++r)
      canon[r] = r;
    if (splits) {
      DenseMap<unsigned, SmallVector<unsigned>> byOld;
      for (unsigned r = 0; r < rawOld.size(); ++r)
        byOld[rawOld[r]].push_back(r);
      auto same = [&](Value a, Value b) {
        auto x = vn.find(a), y = vn.find(b);
        return x != vn.end() && y != vn.end() && x->second == y->second;
      };
      unsigned calls = 0;
      for (auto &[old, rs] : byOld) {
        if (rs.size() < 2)
          continue;
        SmallVector<unsigned> heads; // canonical raw classes so far
        for (unsigned r : rs) {
          bool merged = false;
          for (unsigned h : llvm::reverse(heads)) {
            if (calls >= 100000)
              break;
            ++calls;
            seq::detail::Context ctx(budget, depth, &kb);
            if (seq::detail::equalUnder(ctx, regs[rawFirst[r]].getInput(),
                                        regs[rawFirst[h]].getInput(), splits,
                                        same)) {
              canon[r] = h;
              merged = true;
              ++equalUnderMerges;
              break;
            }
          }
          if (!merged)
            heads.push_back(r);
        }
      }
    }
    // Dense numbering and the join of the known bits.
    SmallVector<unsigned> dense(rawOld.size(), ~0u);
    SmallVector<unsigned> next(regs.size());
    SmallVector<std::optional<KnownBits>> meet;
    for (auto [i, reg] : llvm::enumerate(regs)) {
      unsigned c = canon[raw[i]];
      if (dense[c] == ~0u) {
        dense[c] = meet.size();
        meet.push_back(std::nullopt);
      }
      next[i] = dense[c];
      KnownBits k = knownOf(reg.getInput());
      auto &m = meet[next[i]];
      if (!m)
        m = k;
      else {
        KnownBits u = m->unionWith(k);
        m = u.hasConflict() ? m->intersectWith(k) : u;
      }
    }
    SmallVector<KnownBits> nextKnown(meet.size(), KnownBits(1));
    bool changed = meet.size() != known.size();
    for (auto [i, reg] : llvm::enumerate(regs)) {
      KnownBits k = known[cls[i]].intersectWith(*meet[next[i]]);
      if (k != known[cls[i]])
        changed = true;
      nextKnown[next[i]] = k;
    }
    // Classes not in regs (none) keep nothing.
    cls = next;
    known = nextKnown;
    if (!changed)
      break;
  }
  numRounds += rounds;
  numEqualUnder += equalUnderMerges;

  // Representatives: registers of a class, leaves.
  DenseMap<unsigned, Value> repr;
  SmallVector<int> firstOf(known.size(), -1);
  for (auto [i, reg] : llvm::enumerate(regs))
    if (firstOf[cls[i]] < 0)
      firstOf[cls[i]] = i;
  for (unsigned c = 0; c < known.size(); ++c) {
    Value v = regs[firstOf[c]].getResult();
    repr.try_emplace(vn.lookup(v), v);
  }
  for (auto &[t, v] : leafValue)
    if (!v.getDefiningOp() || !cut.contains(v.getDefiningOp()))
      repr.try_emplace(t, v);

  OpBuilder builder(getOperation().getContext());
  unsigned mergedValues = 0, constValues = 0, mergedRegs = 0, regBits = 0;
  for (auto *op : order) {
    if (isa<hw::ConstantOp>(op) || cut.contains(op) ||
        op->getNumResults() != 1 || !widthOf(op->getResult(0)))
      continue;
    Value v = op->getResult(0);
    unsigned t = vn.lookup(v);
    if (terms.isConst(t)) {
      if (!v.use_empty()) {
        builder.setInsertionPointAfter(op);
        v.replaceAllUsesWith(
            hw::ConstantOp::create(builder, op->getLoc(), terms.get(t).value));
        ++constValues;
      }
      continue;
    }
    auto [it, inserted] = repr.try_emplace(t, v);
    if (inserted || it->second == v)
      continue;
    if (!v.use_empty()) {
      v.replaceAllUsesWith(it->second);
      ++mergedValues;
    }
  }

  // Merge the registers of each class, then read their known bits as
  // constants.
  SmallVector<Operation *> erased;
  for (auto [i, reg] : llvm::enumerate(regs)) {
    auto first = regs[firstOf[cls[i]]];
    if (first == reg)
      continue;
    reg.getResult().replaceAllUsesWith(first.getResult());
    erased.push_back(reg);
    ++mergedRegs;
  }
  for (unsigned c = 0; c < known.size(); ++c) {
    auto reg = regs[firstOf[c]];
    const KnownBits &k = known[c];
    Value v = reg.getResult();
    APInt mask = k.Zero | k.One;
    if (mask.isZero() || v.use_empty())
      continue;
    unsigned width = widthOf(v);
    Location loc = reg.getLoc();
    builder.setInsertionPointAfter(reg);
    if (mask.isAllOnes()) {
      v.replaceAllUsesWith(
          hw::ConstantOp::create(builder, loc, k.getConstant()));
      regBits += width;
      continue;
    }
    SmallVector<Value> parts;
    SmallPtrSet<Operation *, 8> created;
    for (int hi = width; hi > 0;) {
      bool isKnown = mask[hi - 1];
      int lo = hi - 1;
      while (lo > 0 && mask[lo - 1] == isKnown)
        --lo;
      unsigned len = hi - lo;
      if (isKnown) {
        parts.push_back(
            hw::ConstantOp::create(builder, loc, k.One.extractBits(len, lo)));
        regBits += len;
      } else {
        auto e = comb::ExtractOp::create(builder, loc, v, lo, len);
        parts.push_back(e);
        created.insert(e);
      }
      hi = lo;
    }
    v.replaceAllUsesExcept(comb::ConcatOp::create(builder, loc, parts),
                           created);
  }
  if (getenv("WORD_CONGRUENCE_REPORT")) {
    std::map<unsigned, SmallVector<StringRef>> members;
    for (auto [i, reg] : llvm::enumerate(regs))
      members[cls[i]].push_back(reg.getName().value_or("?"));
    for (auto &[c, names] : members) {
      if (names.size() < 2)
        continue;
      llvm::errs() << "class " << c << ":";
      for (auto n : names)
        llvm::errs() << " " << n;
      llvm::errs() << "\n";
    }
  }
  for (auto *op : erased)
    op->dropAllReferences();
  for (auto *op : erased)
    op->erase();
  numMergedRegisters += mergedRegs;
  numMergedValues += mergedValues;
  numConstantValues += constValues;
  numRegisterBits += regBits;
  if (!mergedRegs && !mergedValues && !constValues && !regBits)
    markAllAnalysesPreserved();
}
