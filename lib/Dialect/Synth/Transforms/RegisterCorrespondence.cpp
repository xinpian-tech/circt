//===- RegisterCorrespondence.cpp - Merge sequentially equivalent registers
//===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Register correspondence (van Eijk, "Sequential equivalence checking based on
// structural similarities", TCAD 2000; the latch part of ABC's `scorr`):
// register bits that are equal, complementary or constant in every reachable
// state are merged. Candidates come from random simulation from the initial
// state; they are proved by induction: they hold in the initial state, and if
// all of them hold in a state, they hold in the next one. Counterexamples to
// the inductive step split the classes until the remaining ones are proved.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Synth/SynthOps.h"
#include "circt/Dialect/Synth/Transforms/SynthPasses.h"
#include "circt/Support/SATSolver.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Matchers.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/Debug.h"
#include <map>
#include <optional>
#include <random>

#define DEBUG_TYPE "synth-register-correspondence"

namespace circt {
namespace synth {
#define GEN_PASS_DEF_REGISTERCORRESPONDENCE
#include "circt/Dialect/Synth/Transforms/SynthPasses.h.inc"
} // namespace synth
} // namespace circt

using namespace circt;
using namespace mlir;
using namespace circt::synth;

namespace {
/// A literal of the bit-level net: node index times two, plus one if
/// negated. Node 0 is the constant false.
using Lit = uint32_t;
static Lit mkLit(unsigned node, bool neg = false) { return node * 2 + neg; }
static unsigned nodeOf(Lit l) { return l >> 1; }
static bool isNeg(Lit l) { return l & 1; }
static constexpr Lit kFalse = 0;

/// The bit-level view of a module: free leaves (inputs, bits of unsupported
/// ops, registers that are not modeled), state bits (register bits) and
/// and-gates. Nodes are created after their fanins, so index order is a
/// topological order.
struct Net {
  enum Kind : uint8_t { Const, Free, State, And };
  SmallVector<Kind> kind{Const};
  /// Fanins of and-gates.
  SmallVector<SmallVector<Lit, 2>> fanins{{}};

  unsigned add(Kind k, SmallVector<Lit, 2> ins = {}) {
    kind.push_back(k);
    fanins.push_back(std::move(ins));
    return kind.size() - 1;
  }
  unsigned size() const { return kind.size(); }
};

/// A register bit that is modeled as state.
struct RegBit {
  seq::CompRegOp op;
  unsigned bit;
  unsigned node;
  Lit next = kFalse;
  std::optional<bool> init;
};

/// A candidate class: members with their phase relative to the class value.
/// A constant class has no representative register (its value is false);
/// otherwise the first member is the representative, with phase false.
struct Class {
  bool constant = false;
  SmallVector<std::pair<unsigned, bool>> members; // (register index, phase)
};

struct Params {
  unsigned numPatterns;
  unsigned numFrames;
  uint64_t seed;
  unsigned maxIterations;
  int conflictLimit;
  std::string satSolver;
};

class Correspondence {
public:
  Correspondence(hw::HWModuleOp module, const Params &p)
      : module(module), opts(p), rng(p.seed) {}

  /// Builds the net. Fails (nothing to do) without modeled registers.
  bool build();
  void simulateCandidates();
  void checkBase();
  /// Proves the classes by induction. False if it gave up.
  bool prove();
  /// Replaces the merged registers; returns the number of merged bits.
  unsigned merge();

  unsigned numIterations = 0, numSATCalls = 0, numUnknown = 0;
  unsigned numCandidates() const {
    unsigned n = 0;
    for (auto &c : classes)
      n += c.members.size() - (c.constant ? 0 : 1);
    return n;
  }

private:
  Lit lit(Value value, unsigned bit);
  /// Evaluates the net on 64 patterns: `vals[node]`, with the leaves and the
  /// state bits already set.
  void eval(SmallVectorImpl<uint64_t> &vals) const;
  uint64_t value(ArrayRef<uint64_t> vals, Lit l) const {
    uint64_t v = vals[nodeOf(l)];
    return isNeg(l) ? ~v : v;
  }
  /// Splits the classes by the next-state values in `vals` (for the
  /// patterns in `mask`). Returns true if a class changed.
  bool refine(ArrayRef<uint64_t> vals, uint64_t mask);
  void dropSingletons();

  hw::HWModuleOp module;
  const Params &opts;
  std::mt19937_64 rng;
  Net net;
  SmallVector<RegBit> regs;
  DenseMap<std::pair<Value, unsigned>, Lit> memo;
  SmallVector<Class> classes;
};
} // namespace

/// The literal of `bit` of `value`, through wiring and and-gates; registers
/// that are modeled are state bits, anything else a free leaf. A free leaf is
/// sound: it only allows more behaviors, so an equivalence proved with it
/// also holds in the module.
Lit Correspondence::lit(Value value, unsigned bit) {
  SmallVector<std::pair<std::pair<Value, unsigned>, bool>> stack;
  stack.push_back({{value, bit}, false});
  DenseSet<std::pair<Value, unsigned>> inProgress;
  while (!stack.empty()) {
    auto top = stack.pop_back_val();
    std::pair<Value, unsigned> key = top.first;
    bool expanded = top.second;
    if (memo.count(key))
      continue;
    Value v = key.first;
    unsigned b = key.second;
    // Wiring: forward to the bit it selects.
    auto forward = [&](Value src, unsigned srcBit) -> bool {
      auto srcKey = std::make_pair(src, srcBit);
      if (auto it = memo.find(srcKey); it != memo.end()) {
        memo[key] = it->second;
        return true;
      }
      if (expanded || inProgress.contains(srcKey)) {
        // A cycle of wiring: free.
        memo[key] = mkLit(net.add(Net::Free));
        return true;
      }
      inProgress.insert(key);
      stack.push_back({key, true});
      stack.push_back({srcKey, false});
      return false;
    };
    APInt c;
    if (matchPattern(v, m_ConstantInt(&c))) {
      memo[key] = c[b] ? kFalse ^ 1 : kFalse;
      continue;
    }
    Operation *op = v.getDefiningOp();
    if (auto ext = dyn_cast_or_null<comb::ExtractOp>(op)) {
      forward(ext.getInput(), ext.getLowBit() + b);
      continue;
    }
    if (auto cat = dyn_cast_or_null<comb::ConcatOp>(op)) {
      unsigned pos = 0;
      for (auto in : llvm::reverse(cat.getInputs())) {
        unsigned w = cast<IntegerType>(in.getType()).getWidth();
        if (b < pos + w) {
          forward(in, b - pos);
          break;
        }
        pos += w;
      }
      continue;
    }
    if (auto rep = dyn_cast_or_null<comb::ReplicateOp>(op)) {
      unsigned w = cast<IntegerType>(rep.getInput().getType()).getWidth();
      forward(rep.getInput(), b % w);
      continue;
    }
    if (auto andOp = dyn_cast_or_null<aig::AndInverterOp>(op)) {
      if (!expanded) {
        if (inProgress.contains(key)) {
          // A combinational cycle: free.
          memo[key] = mkLit(net.add(Net::Free));
          continue;
        }
        inProgress.insert(key);
        stack.push_back({key, true});
        for (auto in : andOp.getInputs())
          stack.push_back({{in, b}, false});
        continue;
      }
      SmallVector<Lit, 2> ins;
      bool zero = false;
      for (auto [in, inv] : llvm::zip(andOp.getInputs(), andOp.getInverted())) {
        auto it = memo.find({in, b});
        Lit l = it == memo.end() ? mkLit(net.add(Net::Free)) : it->second;
        l ^= inv;
        if (l == kFalse)
          zero = true;
        else if (l != (kFalse ^ 1))
          ins.push_back(l);
      }
      if (zero)
        memo[key] = kFalse;
      else if (ins.empty())
        memo[key] = kFalse ^ 1;
      else if (ins.size() == 1)
        memo[key] = ins[0];
      else
        memo[key] = mkLit(net.add(Net::And, std::move(ins)));
      continue;
    }
    // Leaves: state bits are created up front; anything else is free.
    memo[key] = mkLit(net.add(Net::Free));
  }
  return memo.at({value, bit});
}

bool Correspondence::build() {
  // Model the registers on the most common clock, without reset, inner
  // symbol, or a non-constant initial value; the others are free.
  DenseMap<Value, unsigned> clocks;
  SmallVector<seq::CompRegOp> all;
  module.walk([&](seq::CompRegOp op) {
    if (op->getBlock() != module.getBodyBlock())
      return;
    all.push_back(op);
    ++clocks[op.getClk()];
  });
  Value clock;
  unsigned best = 0;
  for (auto [c, n] : clocks)
    if (n > best)
      clock = c, best = n;
  for (auto op : all) {
    auto type = dyn_cast<IntegerType>(op.getType());
    if (!type || op.getClk() != clock || op.getReset() || op.getInnerSymAttr())
      continue;
    std::optional<APInt> init;
    if (auto initial = op.getInitialValue()) {
      auto result = dyn_cast<OpResult>(initial);
      auto initialOp = result ? dyn_cast<seq::InitialOp>(result.getOwner())
                              : seq::InitialOp();
      APInt c;
      if (!initialOp ||
          !matchPattern(initialOp.getBodyBlock()->getTerminator()->getOperand(
                            result.getResultNumber()),
                        m_ConstantInt(&c)))
        continue;
      init = c;
    }
    for (unsigned i = 0, e = type.getWidth(); i < e; ++i) {
      RegBit r;
      r.op = op;
      r.bit = i;
      r.node = net.add(Net::State);
      if (init)
        r.init = (*init)[i];
      memo[{op.getResult(), i}] = mkLit(r.node);
      regs.push_back(r);
    }
  }
  if (regs.empty())
    return false;
  for (auto &r : regs)
    r.next = lit(r.op.getInput(), r.bit);
  LLVM_DEBUG(llvm::dbgs() << "register-correspondence: " << regs.size()
                          << " register bits, " << net.size() << " nodes\n");
  return true;
}

void Correspondence::eval(SmallVectorImpl<uint64_t> &vals) const {
  for (unsigned n = 1, e = net.size(); n < e; ++n) {
    if (net.kind[n] != Net::And)
      continue;
    uint64_t v = ~uint64_t(0);
    for (Lit l : net.fanins[n])
      v &= value(vals, l);
    vals[n] = v;
  }
}

void Correspondence::simulateCandidates() {
  unsigned words = std::max(1u, opts.numPatterns / 64);
  unsigned frames = std::max(1u, unsigned(opts.numFrames));
  // Signature of each register bit: its values in every frame and pattern.
  SmallVector<SmallVector<uint64_t>> sig(regs.size());
  SmallVector<uint64_t> vals(net.size());
  for (unsigned w = 0; w < words; ++w) {
    SmallVector<uint64_t> state(regs.size());
    for (unsigned i = 0; i < regs.size(); ++i)
      state[i] = regs[i].init ? (*regs[i].init ? ~uint64_t(0) : 0) : rng();
    for (unsigned f = 0; f < frames; ++f) {
      for (unsigned n = 1; n < net.size(); ++n)
        if (net.kind[n] == Net::Free)
          vals[n] = rng();
      for (unsigned i = 0; i < regs.size(); ++i) {
        vals[regs[i].node] = state[i];
        sig[i].push_back(state[i]);
      }
      eval(vals);
      for (unsigned i = 0; i < regs.size(); ++i)
        state[i] = value(vals, regs[i].next);
    }
  }
  // Group by signature up to complement: normalize to a first bit of 0.
  // The all-zero signature is the constant class.
  std::map<SmallVector<uint64_t>, unsigned> byKey;
  Class constant;
  constant.constant = true;
  for (unsigned i = 0; i < regs.size(); ++i) {
    bool phase = sig[i][0] & 1;
    SmallVector<uint64_t> key = sig[i];
    if (phase)
      for (auto &x : key)
        x = ~x;
    if (llvm::all_of(key, [](uint64_t x) { return x == 0; })) {
      constant.members.push_back({i, phase});
      continue;
    }
    auto [it, inserted] = byKey.try_emplace(key, classes.size());
    if (inserted)
      classes.push_back({});
    classes[it->second].members.push_back({i, phase});
  }
  // Phases relative to the representative.
  for (auto &c : classes) {
    bool p0 = c.members[0].second;
    for (auto &m : c.members)
      m.second ^= p0;
  }
  if (!constant.members.empty())
    classes.push_back(std::move(constant));
  dropSingletons();
}

void Correspondence::dropSingletons() {
  llvm::erase_if(classes, [](const Class &c) {
    return c.members.size() < (c.constant ? 1u : 2u);
  });
}

/// The initial state must satisfy every class. Members without a constant
/// initial value are dropped; the others are grouped by their initial value
/// relative to the class: those that match the class value (false for a
/// constant class, the first member's otherwise) stay, the rest form a
/// class of their own. (Simulation starts in the initial state, so the
/// groups differ only for members with an unknown initial value.)
void Correspondence::checkBase() {
  SmallVector<Class> out;
  for (auto &c : classes) {
    SmallVector<std::pair<unsigned, bool>> known;
    for (auto m : c.members)
      if (regs[m.first].init)
        known.push_back(m);
    if (known.empty())
      continue;
    bool classInit =
        c.constant ? false : (*regs[known[0].first].init ^ known[0].second);
    Class kept, rest;
    kept.constant = c.constant;
    for (auto [i, p] : known) {
      if ((*regs[i].init ^ p) == classInit)
        kept.members.push_back({i, p});
      else
        rest.members.push_back({i, p});
    }
    for (Class *k : {&kept, &rest}) {
      if (k->members.empty() || k->constant)
        continue;
      bool p0 = k->members[0].second;
      for (auto &m : k->members)
        m.second ^= p0;
    }
    out.push_back(std::move(kept));
    if (!rest.members.empty())
      out.push_back(std::move(rest));
  }
  classes = std::move(out);
  dropSingletons();
}

bool Correspondence::refine(ArrayRef<uint64_t> vals, uint64_t mask) {
  bool changed = false;
  SmallVector<Class> out;
  for (auto &c : classes) {
    // Class value per pattern: the representative's next value, or false.
    uint64_t ref = c.constant ? 0 : value(vals, regs[c.members[0].first].next);
    Class same, other;
    same.constant = c.constant;
    for (auto [i, p] : c.members) {
      uint64_t v = value(vals, regs[i].next);
      if (p)
        v = ~v;
      if (((v ^ ref) & mask) == 0)
        same.members.push_back({i, p});
      else
        other.members.push_back({i, p});
    }
    out.push_back(std::move(same));
    if (other.members.empty())
      continue;
    changed = true;
    // The others are refined again on the remaining patterns; with one
    // pattern they all differ from the class value the same way.
    bool p0 = other.members[0].second;
    for (auto &m : other.members)
      m.second ^= p0;
    out.push_back(std::move(other));
  }
  classes = std::move(out);
  // A split class may not be uniform on all patterns yet: repeat until no
  // class changes (each round splits at least one class).
  if (changed)
    refine(vals, mask);
  dropSingletons();
  return changed;
}

bool Correspondence::prove() {
  while (!classes.empty()) {
    if (numIterations++ >= opts.maxIterations)
      return false;
    auto solver = createSATSolver(opts.satSolver);
    if (!solver)
      return false;
    solver->setConflictLimit(opts.conflictLimit);
    int trueVar = solver->newVar();
    solver->addClause({trueVar});
    // SAT variables of the nodes of frame 0, encoded on demand.
    DenseMap<unsigned, int> var;
    auto encode = [&](Lit root) -> int {
      SmallVector<std::pair<unsigned, bool>> stack{{nodeOf(root), false}};
      while (!stack.empty()) {
        auto [n, expanded] = stack.pop_back_val();
        if (var.count(n))
          continue;
        if (n == 0) {
          var[n] = -trueVar;
          continue;
        }
        if (net.kind[n] != Net::And) {
          var[n] = solver->newVar();
          continue;
        }
        if (!expanded) {
          stack.push_back({n, true});
          for (Lit l : net.fanins[n])
            stack.push_back({nodeOf(l), false});
          continue;
        }
        SmallVector<int> ins;
        for (Lit l : net.fanins[n])
          ins.push_back(isNeg(l) ? -var.at(nodeOf(l)) : var.at(nodeOf(l)));
        int out = solver->newVar();
        addAndClauses(out, ins,
                      [&](ArrayRef<int> cl) { solver->addClause(cl); });
        var[n] = out;
      }
      int v = var.at(nodeOf(root));
      return isNeg(root) ? -v : v;
    };
    // Hypotheses: every class holds in frame 0.
    for (auto &c : classes) {
      int ref =
          c.constant ? -trueVar : encode(mkLit(regs[c.members[0].first].node));
      for (auto [i, p] : c.members) {
        int x = encode(mkLit(regs[i].node));
        if (p)
          x = -x;
        if (x == ref)
          continue;
        solver->addClause({-x, ref});
        solver->addClause({x, -ref});
      }
    }
    // Each class member must equal the class value in frame 1. A
    // counterexample is a state satisfying the hypotheses whose successor
    // does not; it is simulated (with its free leaves) to split classes.
    SmallVector<uint64_t> cexVals(net.size());
    unsigned numCex = 0;
    bool unknown = false;
    SmallVector<unsigned> giveUp;
    auto flush = [&]() {
      if (!numCex)
        return;
      eval(cexVals);
      uint64_t mask = numCex == 64 ? ~uint64_t(0) : (uint64_t(1) << numCex) - 1;
      refine(cexVals, mask);
      numCex = 0;
    };
    bool anyCex = false;
    // Snapshot of the pairs to check: (register index, phase, class index).
    SmallVector<std::tuple<unsigned, bool, unsigned>> todo;
    for (auto [ci, c] : llvm::enumerate(classes))
      for (auto [i, p] : c.members)
        if (c.constant || i != c.members[0].first)
          todo.push_back({i, p, unsigned(ci)});
    auto classesAtStart = classes;
    for (auto [i, p, ci] : todo) {
      auto &c = classesAtStart[ci];
      Lit refNext = c.constant ? kFalse : regs[c.members[0].first].next;
      Lit mNext = regs[i].next ^ p;
      int a = encode(mNext), b = encode(refNext);
      if (a == b)
        continue;
      int diff = solver->newVar();
      // diff -> a xor b
      solver->addClause({-diff, a, b});
      solver->addClause({-diff, -a, -b});
      ++numSATCalls;
      auto r = solver->solve({diff});
      if (r == IncrementalSATSolver::kUNSAT) {
        solver->addClause({-diff});
        continue;
      }
      if (r != IncrementalSATSolver::kSAT) {
        ++numUnknown;
        unknown = true;
        giveUp.push_back(i);
        continue;
      }
      anyCex = true;
      // Record the counterexample as pattern numCex of frame 0.
      uint64_t bitMask = uint64_t(1) << numCex;
      for (unsigned n = 1; n < net.size(); ++n) {
        if (net.kind[n] == Net::And)
          continue;
        bool v = false;
        if (auto it = var.find(n); it != var.end())
          v = solver->val(std::abs(it->second)) > 0;
        if (v)
          cexVals[n] |= bitMask;
        else
          cexVals[n] &= ~bitMask;
      }
      if (++numCex == 64)
        flush();
    }
    flush();
    // A member whose check gave up is removed from its class.
    if (unknown) {
      DenseSet<unsigned> drop(giveUp.begin(), giveUp.end());
      for (auto &c : classes) {
        if (!c.constant && drop.contains(c.members[0].first)) {
          // Removing the representative: the rest still form a class
          // relative to it, so keep the phases relative to the new first.
          c.members.erase(c.members.begin());
          if (!c.members.empty()) {
            bool p0 = c.members[0].second;
            for (auto &m : c.members)
              m.second ^= p0;
          }
          continue;
        }
        llvm::erase_if(c.members,
                       [&](auto &m) { return drop.contains(m.first); });
      }
      dropSingletons();
    }
    if (!anyCex && !unknown)
      return true;
  }
  return true;
}

unsigned Correspondence::merge() {
  // Split the registers involved into one register per bit.
  llvm::SetVector<Operation *> involved;
  for (auto &c : classes)
    for (auto [i, p] : c.members)
      involved.insert(regs[i].op);
  DenseMap<std::pair<Operation *, unsigned>, Value> bitValue;
  DenseMap<IntegerAttr, Value> initials;
  for (auto *opPtr : involved) {
    auto op = cast<seq::CompRegOp>(opPtr);
    unsigned w = cast<IntegerType>(op.getType()).getWidth();
    if (w == 1) {
      bitValue[{op, 0}] = op.getResult();
      continue;
    }
    OpBuilder builder(op);
    auto loc = op.getLoc();
    std::optional<APInt> init;
    if (auto initial = op.getInitialValue()) {
      auto result = cast<OpResult>(initial);
      auto initialOp = cast<seq::InitialOp>(result.getOwner());
      APInt c;
      (void)matchPattern(initialOp.getBodyBlock()->getTerminator()->getOperand(
                             result.getResultNumber()),
                         m_ConstantInt(&c));
      init = c;
    }
    SmallVector<Value> bits;
    SmallVector<seq::CompRegOp> newRegs;
    for (unsigned i = 0; i < w; ++i) {
      Value next = comb::ExtractOp::create(builder, loc, op.getInput(), i, 1);
      Value initValue;
      if (init) {
        auto attr = builder.getIntegerAttr(builder.getI1Type(), (*init)[i]);
        auto &v = initials[attr];
        if (!v) {
          OpBuilder::InsertionGuard guard(builder);
          builder.setInsertionPointToStart(module.getBodyBlock());
          v = seq::createConstantInitialValue(builder, loc, attr);
        }
        initValue = v;
      }
      StringAttr name;
      if (auto n = op.getNameAttr())
        name = builder.getStringAttr(n.getValue() + "[" + Twine(i) + "]");
      auto reg =
          seq::CompRegOp::create(builder, loc, next, op.getClk(), Value(),
                                 Value(), name, hw::InnerSymAttr(), initValue);
      bits.push_back(reg);
      bitValue[{op, i}] = reg;
    }
    SmallVector<Value> msbFirst(llvm::reverse(bits));
    Value whole = comb::ConcatOp::create(builder, loc, msbFirst);
    op.replaceAllUsesWith(whole);
    op.erase();
  }
  // Replace the members by the class value.
  unsigned merged = 0;
  OpBuilder builder(module.getContext());
  for (auto &c : classes) {
    Value ref;
    if (!c.constant)
      ref = bitValue.at(
          {regs[c.members[0].first].op, regs[c.members[0].first].bit});
    for (auto [i, p] : c.members) {
      Value v = bitValue.at({regs[i].op, regs[i].bit});
      if (v == ref)
        continue;
      auto reg = v.getDefiningOp<seq::CompRegOp>();
      builder.setInsertionPointAfter(reg);
      Value repl;
      if (c.constant)
        repl = hw::ConstantOp::create(
            builder, reg.getLoc(),
            builder.getIntegerAttr(builder.getI1Type(), p));
      else if (p) {
        builder.setInsertionPointToEnd(module.getBodyBlock());
        builder.setInsertionPoint(module.getBodyBlock()->getTerminator());
        repl = aig::AndInverterOp::create(builder, reg.getLoc(), ref, true);
      } else
        repl = ref;
      reg.getResult().replaceAllUsesWith(repl);
      reg.erase();
      ++merged;
    }
  }
  return merged;
}

namespace {
struct RegisterCorrespondencePass
    : public circt::synth::impl::RegisterCorrespondenceBase<
          RegisterCorrespondencePass> {
  using RegisterCorrespondenceBase::RegisterCorrespondenceBase;

  void runOnOperation() override {
    auto module = getOperation();
    Params opts{numPatterns,   numFrames,          seed,
                maxIterations, int(conflictLimit), satSolver};
    Correspondence c(module, opts);
    if (!c.build())
      return markAllAnalysesPreserved();
    c.simulateCandidates();
    c.checkBase();
    numCandidatesStat += c.numCandidates();
    bool proved = c.prove();
    numIterationsStat += c.numIterations;
    numSATCallsStat += c.numSATCalls;
    numUnknownStat += c.numUnknown;
    if (!proved) {
      module.emitWarning("register correspondence gave up after ")
          << c.numIterations << " iterations; nothing merged";
      return markAllAnalysesPreserved();
    }
    numMergedStat += c.merge();
  }
};
} // namespace
