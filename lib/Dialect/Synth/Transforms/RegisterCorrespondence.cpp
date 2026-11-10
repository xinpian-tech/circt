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
// With `internal`, i1 and-gates are candidates too (signal correspondence):
// the base case is then checked with SAT for all inputs of the initial
// state, and the inductive step over two frames.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombDialect.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Synth/SynthDialect.h"
#include "circt/Dialect/Synth/SynthOps.h"
#include "circt/Dialect/Synth/Transforms/SynthPasses.h"
#include "circt/Dialect/Verif/VerifOps.h"
#include "circt/Support/Namespace.h"
#include "circt/Support/SATSolver.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Matchers.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/Debug.h"
#include <functional>
#include <map>
#include <optional>
#include <random>

#define DEBUG_TYPE "synth-register-correspondence"

namespace circt {
namespace synth {
#define GEN_PASS_DEF_REGISTERCORRESPONDENCE
#define GEN_PASS_DEF_TERNARYCONSTANTS
#define GEN_PASS_DEF_CHECKCONSTRAINTS
#define GEN_PASS_DEF_TEMPORALDECOMPOSITION
#include "circt/Dialect/Synth/Transforms/SynthPasses.h.inc"
} // namespace synth
} // namespace circt

using namespace circt;
using namespace mlir;
using namespace circt::synth;

/// The constant initial value of a register: true with `init` set if it has
/// one, true with `init` empty if it has none, false if it is not constant.
static bool getConstantInit(seq::CompRegOp op, std::optional<APInt> &init) {
  init.reset();
  auto initial = op.getInitialValue();
  if (!initial)
    return true;
  APInt c;
  if (!initial.getDefiningOp<seq::InitialOp>() ||
      !matchPattern(seq::unwrapImmutableValue(initial), m_ConstantInt(&c)))
    return false;
  init = c;
  return true;
}

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

/// A candidate: a register bit (`reg` >= 0) or, with `internal`, an i1
/// and-gate of the module (`value`); `node` is its node in the net.
struct Cand {
  unsigned node;
  int reg = -1;
  Value value;
};

/// A candidate class: members (candidate index, phase relative to the class
/// value). A constant class has no representative (its value is false);
/// otherwise the first member is the representative, with phase false.
/// Members are kept in candidate order: registers first, then and-gates in
/// topological order, so the representative never depends on a member.
struct Class {
  bool constant = false;
  SmallVector<std::pair<unsigned, bool>> members;
};

struct Params {
  unsigned numPatterns;
  unsigned numFrames;
  uint64_t seed;
  unsigned maxIterations;
  int conflictLimit;
  std::string satSolver;
  bool internal;
  unsigned cexFrames;
  unsigned chunk;
};

class Correspondence {
public:
  Correspondence(hw::HWModuleOp module, const Params &p)
      : module(module), opts(p), rng(p.seed) {}

  /// Builds the net. Fails (nothing to do) without modeled registers.
  bool build();
  void simulateCandidates();
  /// Proves that the classes hold in the initial state (for all inputs).
  /// False if it gave up.
  bool proveBase();
  /// Proves the classes by induction. False if it gave up.
  bool proveStep();
  /// The base case (`step` false) or the inductive step, with one
  /// incremental solver: candidate pairs are checked a chunk at a time by
  /// one query whether any of them differs (see the definition).
  bool proveIncremental(bool step);
  /// Finds the registers that are constant in every reachable state by
  /// ternary simulation (inputs and unknown initial values are X) to a
  /// fixpoint, and makes them the (only) class. Returns their number.
  unsigned ternaryConstants();
  /// Replaces the merged candidates; returns the number of merged
  /// (register bits, and-gates).
  std::pair<unsigned, unsigned> merge();

  /// No trace from the initial state satisfies the constraints for `cycles`
  /// cycles; `core` is a subset of the constraints that already conflict.
  struct Conflict {
    unsigned cycles;
    SmallVector<verif::AssumeOp> core;
  };
  /// Checks the constraints (i1 `verif.assume`s of the body) for up to
  /// `depth` cycles. `gaveUp` is set if a check hit the conflict limit.
  std::optional<Conflict> checkConstraints(unsigned depth, bool &gaveUp);

  struct Decomposition {
    /// Cycles removed from the start (0: nothing shifted).
    unsigned prefix = 0;
    /// Register bits that are constant from the start, and those that are
    /// constant only after the prefix.
    unsigned constant = 0, transient = 0;
    /// Inputs added for the cycles of the prefix.
    unsigned inputs = 0;
    bool changed = false;
  };
  /// Temporal decomposition (see the definition).
  Decomposition decompose(unsigned maxPrefix, bool checkPrefix, bool badOutputs,
                          bool force);

  unsigned numIterations = 0, numSATCalls = 0, numUnknown = 0;
  unsigned numCandidates() const {
    unsigned n = 0;
    for (auto &c : classes)
      n += c.members.size() - (c.constant ? 0 : 1);
    return n;
  }

private:
  Lit lit(Value value, unsigned bit);
  /// Evaluates the and-gates on 64 patterns: `vals[node]`, with the leaves
  /// and the state bits already set.
  void eval(SmallVectorImpl<uint64_t> &vals) const;
  uint64_t value(ArrayRef<uint64_t> vals, Lit l) const {
    uint64_t v = vals[nodeOf(l)];
    return isNeg(l) ? ~v : v;
  }
  /// Splits the classes by the candidate values in `vals` (for the patterns
  /// in `mask`).
  void refine(ArrayRef<uint64_t> vals, uint64_t mask);
  void dropSingletons();
  /// Removes candidates from their classes (checks that gave up).
  void drop(const DenseSet<unsigned> &cands);
  /// The speculative reduction of the current classes.
  DenseMap<unsigned, struct Subst> reduction() const;
  /// Simulates the speculatively reduced net: `own[node]` is the value of
  /// each node's own function (the leaves already set), with the fanins
  /// reduced as in `subst`.
  void evalReduced(SmallVectorImpl<uint64_t> &own,
                   const DenseMap<unsigned, struct Subst> &subst) const;
  /// One round of checks of every member's own function (`memberVar`)
  /// against its class value (`refVar`) in the checked frame;
  /// counterexamples are recorded as leaf patterns (`record`), simulated
  /// (`simulate`, to the own values of the checked frame) and split the
  /// classes. Returns false if a check found a counterexample or gave up.
  bool checkRound(
      IncrementalSATSolver &solver,
      llvm::function_ref<int(unsigned node)> memberVar,
      llvm::function_ref<int(unsigned node)> refVar,
      llvm::function_ref<void(SmallVectorImpl<uint64_t> &vals, uint64_t bit)>
          record,
      llvm::function_ref<void(SmallVectorImpl<uint64_t> &vals)> simulate);

  hw::HWModuleOp module;
  const Params &opts;
  std::mt19937_64 rng;
  Net net;
  SmallVector<RegBit> regs;
  /// The register of each state node.
  DenseMap<unsigned, unsigned> regOfNode;
  SmallVector<Cand> cands;
  DenseMap<std::pair<Value, unsigned>, Lit> memo;
  SmallVector<Class> classes;
  /// Later frames simulated from a counterexample of the inductive step,
  /// which also split the classes (see proveStep).
  SmallVector<SmallVector<uint64_t>> extraFrames;
  /// Ternary values of all nodes for a ternary state (one value per
  /// register); inputs are X.
  void ternaryEval(ArrayRef<uint8_t> state,
                   SmallVectorImpl<uint8_t> &vals) const;
  /// The ternary state after the cycle evaluated in `vals`.
  SmallVector<uint8_t> ternaryNext(ArrayRef<uint8_t> vals) const;

  /// The value and bit that each free leaf stands for (leaves made up to
  /// break cycles have none).
  DenseMap<unsigned, std::pair<Value, unsigned>> leafOrigin;
  /// Whether a free leaf was made up (to break a cycle): the net then has
  /// more behaviors than the module.
  bool approximate = false;
  /// Whether the net has free leaves that are results of ops in the body
  /// (not inputs or registers): an and-gate may then depend on another one
  /// through them, which the net does not show.
  bool hiddenPaths = false;
};

/// Speculative reduction: a class member takes the value of its class (its
/// representative, inverted by the phase, or the constant false inverted).
struct Subst {
  bool constant;
  unsigned node;
  bool phase;
};

/// SAT encoding of the nodes of one frame, on demand, speculatively reduced:
/// `node` gives the value of a node as its fanins see it (the class value for
/// a class member), `own` the value of its own function.
class FrameEncoder {
public:
  FrameEncoder(IncrementalSATSolver &solver, const Net &net, int trueVar,
               std::function<int(unsigned node)> leaf,
               const DenseMap<unsigned, Subst> &subst)
      : solver(solver), net(net), trueVar(trueVar), leaf(std::move(leaf)),
        subst(subst) {}

  int operator()(Lit l) {
    int v = node(nodeOf(l));
    return isNeg(l) ? -v : v;
  }

  int node(unsigned root) {
    SmallVector<std::pair<unsigned, bool>> stack{{root, false}};
    while (!stack.empty()) {
      auto [n, expanded] = stack.pop_back_val();
      if (var.count(n))
        continue;
      if (n == 0) {
        var[n] = -trueVar;
        continue;
      }
      if (auto it = subst.find(n); it != subst.end()) {
        auto s = it->second;
        if (s.constant) {
          var[n] = s.phase ? trueVar : -trueVar;
          continue;
        }
        if (!expanded) {
          stack.push_back({n, true});
          stack.push_back({s.node, false});
          continue;
        }
        int r = var.at(s.node);
        var[n] = s.phase ? -r : r;
        continue;
      }
      if (net.kind[n] != Net::And) {
        var[n] = leaf(n);
        continue;
      }
      if (!expanded) {
        stack.push_back({n, true});
        for (Lit l : net.fanins[n])
          stack.push_back({nodeOf(l), false});
        continue;
      }
      var[n] = gate(n);
    }
    return var.at(root);
  }

  /// The value of the node's own function: its gate over the (reduced)
  /// values of its fanins, or its leaf value.
  int own(unsigned n) {
    if (!subst.count(n))
      return node(n);
    if (auto it = ownVar.find(n); it != ownVar.end())
      return it->second;
    int v;
    if (net.kind[n] == Net::And) {
      for (Lit l : net.fanins[n])
        node(nodeOf(l));
      v = gate(n);
    } else {
      v = leaf(n);
    }
    ownVar[n] = v;
    return v;
  }

  /// The SAT variable of a node if it was encoded.
  std::optional<int> lookup(unsigned n) const {
    auto it = var.find(n);
    if (it == var.end())
      return std::nullopt;
    return it->second;
  }

  /// The SAT variable of a node's own function if it was encoded.
  std::optional<int> lookupOwn(unsigned n) const {
    auto it = ownVar.find(n);
    if (it == ownVar.end())
      return lookup(n);
    return it->second;
  }

private:
  /// A new variable for the and-gate n over its encoded fanins.
  int gate(unsigned n) {
    SmallVector<int> ins;
    for (Lit l : net.fanins[n])
      ins.push_back(isNeg(l) ? -var.at(nodeOf(l)) : var.at(nodeOf(l)));
    int out = solver.newVar();
    addAndClauses(out, ins, [&](ArrayRef<int> cl) { solver.addClause(cl); });
    return out;
  }

  IncrementalSATSolver &solver;
  const Net &net;
  int trueVar;
  std::function<int(unsigned node)> leaf;
  const DenseMap<unsigned, Subst> &subst;
  DenseMap<unsigned, int> var, ownVar;
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
        approximate = true;
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
          approximate = true;
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
        if (it == memo.end())
          approximate = true;
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
    unsigned leaf = net.add(Net::Free);
    leafOrigin[leaf] = key;
    memo[key] = mkLit(leaf);
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
  // Ties go to the clock of the first register (deterministic).
  Value clock;
  unsigned best = 0;
  for (auto op : all)
    if (unsigned n = clocks.lookup(op.getClk()); n > best)
      clock = op.getClk(), best = n;
  for (auto op : all) {
    auto type = dyn_cast<IntegerType>(op.getType());
    if (!type || op.getClk() != clock || op.getReset() || op.getInnerSymAttr())
      continue;
    std::optional<APInt> init;
    if (!getConstantInit(op, init))
      continue;
    for (unsigned i = 0, e = type.getWidth(); i < e; ++i) {
      RegBit r;
      r.op = op;
      r.bit = i;
      r.node = net.add(Net::State);
      if (init)
        r.init = (*init)[i];
      memo[{op.getResult(), i}] = mkLit(r.node);
      regOfNode[r.node] = regs.size();
      regs.push_back(r);
    }
  }
  if (regs.empty())
    return false;
  for (auto &r : regs)
    r.next = lit(r.op.getInput(), r.bit);
  for (auto [i, r] : llvm::enumerate(regs))
    cands.push_back({r.node, int(i), r.op.getResult()});
  if (opts.internal) {
    // The i1 and-gates of the module, in topological order.
    DenseSet<unsigned> seen;
    SmallVector<Cand> gates;
    for (auto op : module.getBodyBlock()->getOps<aig::AndInverterOp>()) {
      if (!op.getType().isInteger(1))
        continue;
      Lit l = lit(op.getResult(), 0);
      unsigned n = nodeOf(l);
      if (isNeg(l) || net.kind[n] != Net::And || !seen.insert(n).second)
        continue;
      gates.push_back({n, -1, op.getResult()});
    }
    llvm::sort(gates,
               [](const Cand &a, const Cand &b) { return a.node < b.node; });
    cands.append(gates.begin(), gates.end());
  }
  // Free leaves that are results of ops in the body (not inputs or
  // registers) may hide paths between and-gates.
  for (auto &[key, l] : memo) {
    auto result = dyn_cast<OpResult>(key.first);
    if (result && net.kind[nodeOf(l)] == Net::Free &&
        !isa<seq::CompRegOp>(result.getOwner()))
      hiddenPaths = true;
  }
  LLVM_DEBUG(llvm::dbgs() << "register-correspondence: " << regs.size()
                          << " register bits, " << cands.size()
                          << " candidates, " << net.size() << " nodes\n");
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
  // Signature of each candidate: its values in every frame and pattern.
  SmallVector<SmallVector<uint64_t>> sig(cands.size());
  SmallVector<uint64_t> vals(net.size());
  for (unsigned w = 0; w < words; ++w) {
    SmallVector<uint64_t> state(regs.size());
    for (unsigned i = 0; i < regs.size(); ++i)
      state[i] = regs[i].init ? (*regs[i].init ? ~uint64_t(0) : 0) : rng();
    for (unsigned f = 0; f < frames; ++f) {
      for (unsigned n = 1; n < net.size(); ++n)
        if (net.kind[n] == Net::Free)
          vals[n] = rng();
      for (unsigned i = 0; i < regs.size(); ++i)
        vals[regs[i].node] = state[i];
      eval(vals);
      for (unsigned c = 0; c < cands.size(); ++c)
        sig[c].push_back(vals[cands[c].node]);
      for (unsigned i = 0; i < regs.size(); ++i)
        state[i] = value(vals, regs[i].next);
    }
  }
  // Group by signature up to complement: normalize to a first bit of 0.
  // The all-zero signature is the constant class.
  std::map<SmallVector<uint64_t>, unsigned> byKey;
  Class constant;
  constant.constant = true;
  for (unsigned i = 0; i < cands.size(); ++i) {
    bool phase = sig[i][0] & 1;
    SmallVector<uint64_t> key = std::move(sig[i]);
    if (phase)
      for (auto &x : key)
        x = ~x;
    if (llvm::all_of(key, [](uint64_t x) { return x == 0; })) {
      constant.members.push_back({i, phase});
      continue;
    }
    auto [it, inserted] = byKey.try_emplace(std::move(key), classes.size());
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

void Correspondence::drop(const DenseSet<unsigned> &gone) {
  for (auto &c : classes) {
    if (!c.constant && !c.members.empty() &&
        gone.contains(c.members[0].first)) {
      // Without the representative, the rest is relative to the next one.
      c.members.erase(c.members.begin());
      if (!c.members.empty()) {
        bool p0 = c.members[0].second;
        for (auto &m : c.members)
          m.second ^= p0;
      }
    }
    llvm::erase_if(c.members, [&](auto &m) { return gone.contains(m.first); });
  }
  dropSingletons();
}

void Correspondence::refine(ArrayRef<uint64_t> vals, uint64_t mask) {
  // Each class splits by the members' values relative to the class value:
  // members with equal relative values stay together. The group of the
  // representative (relative value 0) keeps the class; a constant class has
  // the value 0 too.
  SmallVector<Class> out;
  for (auto &c : classes) {
    uint64_t ref = c.constant ? 0 : vals[cands[c.members[0].first].node];
    llvm::MapVector<uint64_t, Class> groups;
    for (auto [i, p] : c.members) {
      uint64_t v = vals[cands[i].node];
      if (p)
        v = ~v;
      groups[(v ^ ref) & mask].members.push_back({i, p});
    }
    for (auto &[key, g] : groups) {
      if (key == 0) {
        g.constant = c.constant;
      } else {
        // A new class: relative to its first member.
        bool p0 = g.members[0].second;
        for (auto &m : g.members)
          m.second ^= p0;
      }
      out.push_back(std::move(g));
    }
  }
  classes = std::move(out);
  dropSingletons();
}

DenseMap<unsigned, Subst> Correspondence::reduction() const {
  DenseMap<unsigned, Subst> subst;
  for (auto &c : classes)
    for (auto [i, p] : c.members) {
      if (!c.constant && i == c.members[0].first)
        continue;
      subst[cands[i].node] = {c.constant, cands[c.members[0].first].node, p};
    }
  return subst;
}

void Correspondence::evalReduced(SmallVectorImpl<uint64_t> &own,
                                 const DenseMap<unsigned, Subst> &subst) const {
  // red[n]: the value fanins see; own[n]: the node's own function.
  SmallVector<uint64_t> red(own.begin(), own.end());
  for (unsigned n = 1, e = net.size(); n < e; ++n) {
    if (net.kind[n] == Net::And) {
      uint64_t v = ~uint64_t(0);
      for (Lit l : net.fanins[n])
        v &= value(red, l);
      own[n] = v;
    }
    red[n] = own[n];
    // Representatives come before their members (registers first, then
    // gates in topological order), so red of the representative is final.
    if (auto it = subst.find(n); it != subst.end()) {
      auto s = it->second;
      uint64_t r = s.constant ? 0 : red[s.node];
      red[n] = s.phase ? ~r : r;
    }
  }
}

bool Correspondence::checkRound(
    IncrementalSATSolver &solver,
    llvm::function_ref<int(unsigned node)> memberVar,
    llvm::function_ref<int(unsigned node)> refVar,
    llvm::function_ref<void(SmallVectorImpl<uint64_t> &, uint64_t)> record,
    llvm::function_ref<void(SmallVectorImpl<uint64_t> &)> simulate) {
  SmallVector<uint64_t> leaves(2 * net.size());
  SmallVector<uint64_t> out;
  unsigned numCex = 0;
  bool clean = true;
  DenseSet<unsigned> giveUp;
  // The class value of each candidate in `classes`: its representative, or
  // -1 for the constant class (-2: in no class).
  SmallVector<int> current(cands.size(), -2);
  auto index = [&]() {
    std::fill(current.begin(), current.end(), -2);
    for (auto &c : classes)
      for (auto [i, p] : c.members)
        current[i] = c.constant ? -1 : int(c.members[0].first);
  };
  index();
  auto flush = [&]() {
    if (!numCex)
      return;
    out = leaves;
    extraFrames.clear();
    simulate(out);
    uint64_t mask = numCex == 64 ? ~uint64_t(0) : (uint64_t(1) << numCex) - 1;
    refine(out, mask);
    for (auto &frame : extraFrames)
      refine(frame, mask);
    numCex = 0;
    index();
  };
  // The classes at the start of the round (the encoding's hypotheses);
  // counterexamples refine `classes`.
  auto snapshot = classes;
  for (auto &c : snapshot) {
    int refCand = c.constant ? -1 : int(c.members[0].first);
    int ref = refVar(c.constant ? 0 : cands[refCand].node);
    for (auto [i, p] : c.members) {
      if (int(i) == refCand)
        continue;
      // Already split from its class by a counterexample of this round.
      if (current[i] != refCand)
        continue;
      int x = memberVar(cands[i].node);
      if (p)
        x = -x;
      if (x == ref)
        continue;
      int diff = solver.newVar();
      solver.addClause({-diff, x, ref});
      solver.addClause({-diff, -x, -ref});
      ++numSATCalls;
      auto r = solver.solve({diff});
      if (r == IncrementalSATSolver::kUNSAT) {
        solver.addClause({-diff});
        continue;
      }
      clean = false;
      if (r != IncrementalSATSolver::kSAT) {
        ++numUnknown;
        giveUp.insert(i);
        continue;
      }
      record(leaves, uint64_t(1) << numCex);
      if (++numCex == 64)
        flush();
    }
  }
  flush();
  if (!giveUp.empty())
    drop(giveUp);
  LLVM_DEBUG(llvm::dbgs() << "register-correspondence: round " << numIterations
                          << ": " << snapshot.size() << " classes before, "
                          << classes.size() << " after, " << numCandidates()
                          << " candidates, " << numSATCalls << " SAT calls\n");
  return clean;
}

/// The value of a SAT literal in the solver's model.
static bool modelValue(IncrementalSATSolver &solver, int lit) {
  bool v = solver.val(std::abs(lit)) > 0;
  return lit > 0 ? v : !v;
}

static void setBit(uint64_t &word, uint64_t bit, bool v) {
  word = v ? (word | bit) : (word & ~bit);
}

bool Correspondence::proveBase() {
  while (!classes.empty()) {
    if (numIterations++ >= opts.maxIterations)
      return false;
    auto solver = createSATSolver(opts.satSolver);
    if (!solver)
      return false;
    solver->setConflictLimit(opts.conflictLimit);
    int trueVar = solver->newVar();
    solver->addClause({trueVar});
    auto subst = reduction();
    // Frame 0 in the initial state: registers with an initial value are
    // constants, the others and the inputs are free.
    auto leaf = [&](unsigned n) -> int {
      if (net.kind[n] == Net::State)
        if (auto init = regs[regOfNode.at(n)].init)
          return *init ? trueVar : -trueVar;
      return solver->newVar();
    };
    FrameEncoder enc(*solver, net, trueVar, leaf, subst);
    auto record = [&](SmallVectorImpl<uint64_t> &leaves, uint64_t bit) {
      for (unsigned n = 1; n < net.size(); ++n) {
        if (net.kind[n] == Net::And)
          continue;
        // The own value of a leaf (a register member's own initial value).
        // A register that is not encoded has its initial value, if any (the
        // buffer holds older counterexamples); other leaves can be anything.
        if (auto v = enc.lookupOwn(n))
          setBit(leaves[n], bit, modelValue(*solver, *v));
        else if (net.kind[n] == Net::State)
          if (auto init = regs[regOfNode.at(n)].init)
            setBit(leaves[n], bit, *init);
      }
    };
    auto simulate = [&](SmallVectorImpl<uint64_t> &vals) {
      vals.resize(net.size());
      evalReduced(vals, subst);
    };
    if (checkRound(
            *solver, [&](unsigned n) { return enc.own(n); },
            [&](unsigned n) { return enc.node(n); }, record, simulate))
      return true;
  }
  return true;
}

bool Correspondence::proveStep() {
  while (!classes.empty()) {
    if (numIterations++ >= opts.maxIterations)
      return false;
    auto solver = createSATSolver(opts.satSolver);
    if (!solver)
      return false;
    solver->setConflictLimit(opts.conflictLimit);
    int trueVar = solver->newVar();
    solver->addClause({trueVar});
    auto subst = reduction();
    // Frame 0: any state satisfying the classes (speculatively reduced);
    // frame 1: its successor, with new inputs.
    auto leaf0 = [&](unsigned) { return solver->newVar(); };
    FrameEncoder enc0(*solver, net, trueVar, leaf0, subst);
    auto leaf1 = [&](unsigned n) -> int {
      if (net.kind[n] == Net::State)
        return enc0(regs[regOfNode.at(n)].next);
      return solver->newVar();
    };
    FrameEncoder enc1(*solver, net, trueVar, leaf1, subst);
    // The leaves of frame 0 at [n], the free leaves of frame 1 at
    // [size + n].
    unsigned size = net.size();
    auto record = [&](SmallVectorImpl<uint64_t> &leaves, uint64_t bit) {
      for (unsigned n = 1; n < size; ++n) {
        if (net.kind[n] == Net::And)
          continue;
        if (auto v = enc0.lookup(n))
          setBit(leaves[n], bit, modelValue(*solver, *v));
        if (net.kind[n] == Net::Free)
          if (auto v = enc1.lookup(n))
            setBit(leaves[size + n], bit, modelValue(*solver, *v));
      }
    };
    auto simulate = [&](SmallVectorImpl<uint64_t> &vals) {
      // Frame 0, reduced: a register member has its class's value.
      SmallVector<uint64_t> v0(vals.begin(), vals.begin() + size);
      for (unsigned n = 1; n < size; ++n)
        if (auto it = subst.find(n);
            it != subst.end() && net.kind[n] == Net::State) {
          auto s = it->second;
          uint64_t r = s.constant ? 0 : v0[s.node];
          v0[n] = s.phase ? ~r : r;
        }
      evalReduced(v0, subst);
      // The reduced values of frame 0 feed the next states.
      SmallVector<uint64_t> red0(v0);
      for (unsigned n = 1; n < size; ++n)
        if (auto it = subst.find(n); it != subst.end()) {
          auto s = it->second;
          uint64_t r = s.constant ? 0 : red0[s.node];
          red0[n] = s.phase ? ~r : r;
        }
      SmallVector<uint64_t> v1(vals.begin() + size, vals.end());
      for (auto &r : regs)
        v1[r.node] = value(red0, r.next);
      evalReduced(v1, subst);
      vals.assign(v1.begin(), v1.end());
      // A few more frames from the successor, with random inputs. The
      // counterexample state satisfies the current classes, which contain
      // the ones that will be proved (classes only split); those are
      // inductive, so they also hold in these frames, and splitting by them
      // only removes candidates that would not be proved.
      SmallVector<uint64_t> cur(v1);
      for (unsigned f = 0; f < opts.cexFrames; ++f) {
        SmallVector<uint64_t> red(cur);
        for (unsigned n = 1; n < size; ++n)
          if (auto it = subst.find(n); it != subst.end()) {
            auto s = it->second;
            uint64_t r = s.constant ? 0 : red[s.node];
            red[n] = s.phase ? ~r : r;
          }
        SmallVector<uint64_t> next(size);
        for (unsigned n = 1; n < size; ++n)
          if (net.kind[n] == Net::Free)
            next[n] = rng();
        for (auto &r : regs)
          next[r.node] = value(red, r.next);
        evalReduced(next, subst);
        extraFrames.push_back(next);
        cur = std::move(next);
      }
    };
    if (checkRound(
            *solver, [&](unsigned n) { return enc1.own(n); },
            [&](unsigned n) { return enc1.node(n); }, record, simulate))
      return true;
  }
  return true;
}

/// Without speculative reduction, the frames are plain unrollings and one
/// solver serves the whole proof. Each pair (member, class value) has a
/// difference variable in the checked frame and, for the step, a hypothesis
/// selector for its equality in frame 0. A query assumes the hypotheses of
/// the current classes and asks whether some pair of a chunk differs. A
/// counterexample is simulated on 64 patterns (the model, and for the other
/// patterns the same frame-0 state with random inputs in the checked frame:
/// they satisfy the hypotheses too) and splits the classes; the chunk is
/// then queried again. A pass over all chunks without a counterexample
/// proves the classes.
bool Correspondence::proveIncremental(bool step) {
  if (classes.empty())
    return true;
  auto solver = createSATSolver(opts.satSolver);
  if (!solver)
    return false;
  solver->setConflictLimit(opts.conflictLimit);
  int trueVar = solver->newVar();
  solver->addClause({trueVar});
  static const DenseMap<unsigned, Subst> noSubst;
  auto leaf0 = [&](unsigned n) -> int {
    if (!step && net.kind[n] == Net::State)
      if (auto init = regs[regOfNode.at(n)].init)
        return *init ? trueVar : -trueVar;
    return solver->newVar();
  };
  FrameEncoder enc0(*solver, net, trueVar, leaf0, noSubst);
  auto leaf1 = [&](unsigned n) -> int {
    if (net.kind[n] == Net::State)
      return enc0(regs[regOfNode.at(n)].next);
    return solver->newVar();
  };
  FrameEncoder enc1(*solver, net, trueVar, leaf1, noSubst);
  FrameEncoder &checked = step ? enc1 : enc0;

  // A pair: member candidate, representative candidate (-1: constant
  // false), phase.
  using Pair = std::tuple<unsigned, int, bool>;
  struct Vars {
    int hyp = 0, diff = 0;
  };
  std::map<Pair, Vars> vars;
  // The literal of `x == target`-style relations for a pair in a frame.
  auto target = [&](FrameEncoder &enc, int ref, bool phase) {
    int t = ref < 0 ? -trueVar : enc.node(cands[ref].node);
    return phase ? -t : t;
  };
  auto diffVar = [&](const Pair &pr) {
    auto &v = vars[pr];
    if (!v.diff) {
      auto [m, ref, phase] = pr;
      int x = checked.node(cands[m].node), t = target(checked, ref, phase);
      v.diff = solver->newVar();
      solver->addClause({-v.diff, x, t});
      solver->addClause({-v.diff, -x, -t});
    }
    return v.diff;
  };
  auto hypVar = [&](const Pair &pr) {
    auto &v = vars[pr];
    if (!v.hyp) {
      auto [m, ref, phase] = pr;
      int x = enc0.node(cands[m].node), t = target(enc0, ref, phase);
      v.hyp = solver->newVar();
      solver->addClause({-v.hyp, -x, t});
      solver->addClause({-v.hyp, x, -t});
    }
    return v.hyp;
  };
  // The current pair of each candidate (if in a class, not representative).
  SmallVector<std::optional<Pair>> pairOf(cands.size());
  auto index = [&]() {
    std::fill(pairOf.begin(), pairOf.end(), std::nullopt);
    for (auto &c : classes) {
      int ref = c.constant ? -1 : int(c.members[0].first);
      for (auto [i, p] : c.members)
        if (int(i) != ref)
          pairOf[i] = Pair{i, ref, p};
    }
  };
  // The activation literal of the last query, retired (made false) before
  // the next one; not earlier, as that would discard the model.
  int lastAct = 0;
  auto query = [&](ArrayRef<unsigned> members) {
    if (lastAct)
      solver->addClause({-lastAct});
    SmallVector<int> assumptions;
    if (step)
      for (auto &pr : pairOf)
        if (pr)
          assumptions.push_back(hypVar(*pr));
    SmallVector<int> clause;
    int act = solver->newVar();
    clause.push_back(-act);
    for (unsigned m : members)
      clause.push_back(diffVar(*pairOf[m]));
    solver->addClause(clause);
    assumptions.push_back(act);
    ++numSATCalls;
    lastAct = act;
    return solver->solve(assumptions);
  };
  // Simulates the model on 64 patterns and splits the classes.
  auto refineByModel = [&]() {
    unsigned size = net.size();
    auto modelWord = [&](std::optional<int> v) -> uint64_t {
      return v && modelValue(*solver, *v) ? ~uint64_t(0) : 0;
    };
    SmallVector<uint64_t> v0(size), cur;
    for (unsigned n = 1; n < size; ++n) {
      if (net.kind[n] == Net::And)
        continue;
      auto v = enc0.lookup(n);
      if (step) {
        v0[n] = v ? modelWord(v) : rng();
        continue;
      }
      // The base case: the model in pattern 0, any initial state and
      // inputs in the others.
      bool fixed = net.kind[n] == Net::State && regs[regOfNode.at(n)].init;
      uint64_t r =
          fixed ? (*regs[regOfNode.at(n)].init ? ~uint64_t(0) : 0) : rng();
      v0[n] = (r & ~uint64_t(1)) | (modelWord(v) & 1);
      if (fixed)
        v0[n] = r;
    }
    eval(v0);
    if (step) {
      SmallVector<uint64_t> v1(size);
      for (unsigned n = 1; n < size; ++n)
        if (net.kind[n] == Net::Free)
          v1[n] = (rng() & ~uint64_t(1)) | (modelWord(enc1.lookup(n)) & 1);
      for (auto &r : regs)
        v1[r.node] = value(v0, r.next);
      eval(v1);
      cur = std::move(v1);
    } else {
      cur = std::move(v0);
    }
    refine(cur, ~uint64_t(0));
    // Successors with random inputs (see proveStep for why this is sound in
    // the step; in the base case they are reachable states).
    for (unsigned f = 0; f < opts.cexFrames && !classes.empty(); ++f) {
      SmallVector<uint64_t> next(size);
      for (unsigned n = 1; n < size; ++n)
        if (net.kind[n] == Net::Free)
          next[n] = rng();
      for (auto &r : regs)
        next[r.node] = value(cur, r.next);
      eval(next);
      refine(next, ~uint64_t(0));
      cur = std::move(next);
    }
    index();
  };

  index();
  while (!classes.empty()) {
    if (numIterations++ >= opts.maxIterations)
      return false;
    bool clean = true;
    SmallVector<unsigned> order;
    for (unsigned i = 0; i < cands.size(); ++i)
      if (pairOf[i])
        order.push_back(i);
    DenseSet<unsigned> giveUp;
    for (size_t start = 0; start < order.size(); start += opts.chunk) {
      ArrayRef<unsigned> chunk(order);
      chunk = chunk.slice(start,
                          std::min<size_t>(opts.chunk, order.size() - start));
      while (true) {
        SmallVector<unsigned> members;
        for (unsigned m : chunk)
          if (pairOf[m] && !giveUp.contains(m))
            members.push_back(m);
        if (members.empty())
          break;
        auto r = query(members);
        if (r == IncrementalSATSolver::kUNSAT)
          break;
        clean = false;
        if (r == IncrementalSATSolver::kSAT) {
          refineByModel();
          continue;
        }
        // Gave up on the chunk: check its pairs one by one.
        ++numUnknown;
        for (unsigned m : members) {
          if (!pairOf[m])
            continue;
          auto r1 = query({m});
          if (r1 == IncrementalSATSolver::kSAT)
            refineByModel();
          else if (r1 != IncrementalSATSolver::kUNSAT)
            giveUp.insert(m);
        }
        // Every pair has been decided (or given up) under the current
        // hypotheses; querying the chunk again could hit the limit again and
        // loop. `clean` is false, so the next pass checks it again.
        break;
      }
    }
    if (!giveUp.empty()) {
      drop(giveUp);
      index();
    }
    LLVM_DEBUG(llvm::dbgs()
               << "register-correspondence: " << (step ? "step" : "base")
               << " pass " << numIterations << ": " << classes.size()
               << " classes, " << numCandidates() << " candidates, "
               << numSATCalls << " SAT calls\n");
    if (clean)
      return true;
  }
  return true;
}

/// A ternary value as (may be 0, may be 1).
enum : uint8_t { TZero = 1, TOne = 2, TX = 3 };
static uint8_t ternaryNot(uint8_t v) {
  return uint8_t(((v & TZero) ? TOne : 0) | ((v & TOne) ? TZero : 0));
}

void Correspondence::ternaryEval(ArrayRef<uint8_t> state,
                                 SmallVectorImpl<uint8_t> &vals) const {
  vals.assign(net.size(), TX);
  vals[0] = TZero;
  for (unsigned n = 1; n < net.size(); ++n) {
    switch (net.kind[n]) {
    case Net::Const:
      vals[n] = TZero;
      break;
    case Net::Free:
      vals[n] = TX;
      break;
    case Net::State:
      vals[n] = state[regOfNode.at(n)];
      break;
    case Net::And: {
      bool zero = false, one = true;
      for (Lit l : net.fanins[n]) {
        uint8_t v = vals[nodeOf(l)];
        if (isNeg(l))
          v = ternaryNot(v);
        zero |= v == TZero;
        one &= v == TOne;
      }
      vals[n] = zero ? TZero : one ? TOne : TX;
      break;
    }
    }
  }
}

SmallVector<uint8_t> Correspondence::ternaryNext(ArrayRef<uint8_t> vals) const {
  SmallVector<uint8_t> next(regs.size());
  for (auto [i, r] : llvm::enumerate(regs)) {
    uint8_t v = vals[nodeOf(r.next)];
    next[i] = isNeg(r.next) ? ternaryNot(v) : v;
  }
  return next;
}

unsigned Correspondence::ternaryConstants() {
  // The abstract state: each register's possible values in the states seen
  // so far. Joining with the successor until it no longer changes gives an
  // over-approximation of the reachable states.
  SmallVector<uint8_t> state(regs.size());
  for (auto [i, r] : llvm::enumerate(regs))
    state[i] = r.init ? (*r.init ? TOne : TZero) : TX;
  SmallVector<uint8_t> vals;
  for (bool changed = true; changed;) {
    changed = false;
    ternaryEval(state, vals);
    auto next = ternaryNext(vals);
    for (unsigned i = 0; i < regs.size(); ++i) {
      uint8_t joined = state[i] | next[i];
      if (joined != state[i]) {
        state[i] = joined;
        changed = true;
      }
    }
  }
  Class constant;
  constant.constant = true;
  for (auto [i, r] : llvm::enumerate(regs))
    if (state[i] != TX)
      constant.members.push_back({unsigned(i), state[i] == TOne});
  classes.clear();
  unsigned n = constant.members.size();
  if (n)
    classes.push_back(std::move(constant));
  return n;
}

namespace {
/// An i1 signal while logic is built: a value, possibly inverted, or (no
/// value) the constant `neg`.
struct Sig {
  Value v;
  bool neg = false;
  bool isConst() const { return !v; }
  bool isFalse() const { return !v && !neg; }
  bool isTrue() const { return !v && neg; }
  Sig operator~() const { return {v, !neg}; }
};

/// Builds and-inverter logic over signals, folding constants.
class SigBuilder {
public:
  SigBuilder(OpBuilder &builder, Location loc) : builder(builder), loc(loc) {}

  static Sig constant(bool c) { return {Value(), c}; }

  Sig land(ArrayRef<Sig> ins) {
    SmallVector<Value> values;
    SmallVector<bool> inverted;
    for (Sig s : ins) {
      if (s.isFalse())
        return constant(false);
      if (s.isTrue())
        continue;
      values.push_back(s.v);
      inverted.push_back(s.neg);
    }
    if (values.empty())
      return constant(true);
    if (values.size() == 1)
      return {values[0], inverted[0]};
    return {aig::AndInverterOp::create(builder, loc, values, inverted), false};
  }

  Sig lor(ArrayRef<Sig> ins) {
    SmallVector<Sig> negated;
    for (Sig s : ins)
      negated.push_back(~s);
    return ~land(negated);
  }

  /// The signal as a value.
  Value value(Sig s) {
    if (s.isConst()) {
      auto &c = constants[s.neg];
      if (!c)
        c = hw::ConstantOp::create(builder, loc, builder.getI1Type(), s.neg);
      return c;
    }
    if (!s.neg)
      return s.v;
    return aig::AndInverterOp::create(builder, loc, s.v, true);
  }

private:
  OpBuilder &builder;
  Location loc;
  Value constants[2];
};
} // namespace

/// Temporal decomposition (Case, Mony, Baumgartner, Kanzelman, "Enhanced
/// verification by temporal decomposition", FMCAD 2009; ABC `tempor`).
/// Ternary simulation from the initial state (inputs X) gives the state of
/// each cycle; the join of the states from cycle k on tells which register
/// bits are constant from then on (transient before). The module is rebuilt
/// to start at cycle k, the one (up to `maxPrefix`) that shrinks it most
/// (with `force`: the first one with the most such bits): they become
/// constants; the other register bits start with
/// their value of cycle k, the known one, or, where it depends on the inputs
/// of the prefix, the unrolled logic of the prefix over new inputs
/// (`NAME_preT`), selected in the first cycle by a new register `first`.
///
/// The properties of the prefix are kept: with G_t = the constraints hold in
/// the cycles 0..t of the prefix, an assertion p becomes
///   !(first & pf) & (p | m),  pf = OR_t (G_t & !p_t),  m = first & !G_{k-1}
/// (a failure in the prefix shows in the first cycle), a constraint c
/// becomes c | m, and after a prefix that breaks a constraint the trace
/// ends (a new constraint that is false from the second cycle on). So a
/// counterexample of depth d is one of depth at most d + k of the module
/// before, and a bounded proof to depth d covers d + k.
///
/// Exact only if the net is the module: all state modeled, all leaves
/// inputs. Otherwise nothing is done.
Correspondence::Decomposition Correspondence::decompose(unsigned maxPrefix,
                                                        bool checkPrefix,
                                                        bool badOutputs,
                                                        bool force) {
  Decomposition res;
  auto *body = module.getBodyBlock();
  SmallVector<verif::AssertOp> asserts;
  SmallVector<verif::AssumeOp> assumes;
  for (auto &op : *body) {
    if (isa<seq::CompRegOp, seq::InitialOp, hw::ConstantOp, hw::OutputOp>(op))
      continue;
    if (auto a = dyn_cast<verif::AssertOp>(op)) {
      if (!a.getProperty().getType().isInteger(1))
        return res;
      asserts.push_back(a);
      continue;
    }
    if (auto a = dyn_cast<verif::AssumeOp>(op)) {
      if (!a.getProperty().getType().isInteger(1))
        return res;
      assumes.push_back(a);
      continue;
    }
    if (isa<comb::CombDialect, synth::SynthDialect>(op.getDialect()))
      continue;
    return res;
  }
  // Outputs change with the shift; they are only kept as bad-state outputs.
  auto *outputOp = body->getTerminator();
  if (outputOp->getNumOperands() && !badOutputs)
    return res;
  for (auto out : outputOp->getOperands())
    if (!out.getType().isInteger(1))
      return res;

  // The literal of `enable -> property`.
  auto holdLit = [&](Value prop, Value en) -> Lit {
    Lit l = lit(prop, 0);
    if (!en)
      return l;
    Lit e = lit(en, 0);
    if (e == kFalse || l == (kFalse ^ 1))
      return kFalse ^ 1;
    if (e == (kFalse ^ 1))
      return l;
    if (l == kFalse)
      return e ^ 1;
    return mkLit(net.add(Net::And, {e, l ^ 1})) ^ 1;
  };
  SmallVector<Lit> assertLits, assumeLits, outLits;
  for (auto a : asserts)
    assertLits.push_back(holdLit(a.getProperty(), a.getEnable()));
  for (auto a : assumes)
    assumeLits.push_back(holdLit(a.getProperty(), a.getEnable()));
  for (auto out : outputOp->getOperands())
    outLits.push_back(lit(out, 0));

  // The net must be exact.
  if (approximate)
    return res;
  for (unsigned n = 1; n < net.size(); ++n) {
    if (net.kind[n] != Net::Free)
      continue;
    auto it = leafOrigin.find(n);
    if (it == leafOrigin.end() || !isa<BlockArgument>(it->second.first))
      return res;
  }

  // The ternary states of the first cycles, and the values of all nodes in
  // them.
  SmallVector<SmallVector<uint8_t>> states, frames;
  states.emplace_back(regs.size());
  for (auto [i, r] : llvm::enumerate(regs))
    states[0][i] = r.init ? (*r.init ? TOne : TZero) : TX;
  for (unsigned t = 0; t < maxPrefix; ++t) {
    SmallVector<uint8_t> vals;
    ternaryEval(states[t], vals);
    auto next = ternaryNext(vals);
    // A fixed point: all later states are the same.
    if (next == states[t])
      break;
    frames.push_back(std::move(vals));
    states.push_back(std::move(next));
  }
  // The join of the states from cycle i on (an over-approximation).
  auto forever = [&](unsigned i) {
    SmallVector<uint8_t> join(states[i]), vals;
    for (bool changed = true; changed;) {
      changed = false;
      ternaryEval(join, vals);
      auto next = ternaryNext(vals);
      for (unsigned r = 0; r < regs.size(); ++r) {
        uint8_t joined = join[r] | next[r];
        changed |= joined != join[r];
        join[r] = joined;
      }
    }
    return join;
  };
  auto count = [](ArrayRef<uint8_t> state) {
    return unsigned(llvm::count_if(state, [](uint8_t v) { return v != TX; }));
  };
  // The joins of all candidate prefixes; later ones have more constant bits.
  unsigned last = states.size() - 1;
  SmallVector<SmallVector<uint8_t>> joins;
  for (unsigned i = 0; i <= last; ++i)
    joins.push_back(forever(i));
  unsigned k = 0;
  if (force) {
    // The first prefix with the most constant bits.
    while (count(joins[k]) != count(joins[last]))
      ++k;
  } else {
    // The prefix that shrinks the module most: constant register bits and
    // and-gates, less the logic of the prefix.
    auto constantGates = [&](ArrayRef<uint8_t> state) {
      SmallVector<uint8_t> vals;
      ternaryEval(state, vals);
      unsigned n = 0;
      for (unsigned i = 1; i < net.size(); ++i)
        n += net.kind[i] == Net::And && vals[i] != TX;
      return n;
    };
    auto prefixGates = [&](unsigned i) {
      unsigned gates = 1;
      SmallVector<std::pair<unsigned, unsigned>> stack;
      for (unsigned r = 0; r < regs.size(); ++r)
        if (joins[i][r] == TX && states[i][r] == TX) {
          stack.push_back({regs[r].node, i});
          gates += 3;
        }
      for (unsigned t = 0; t < i; ++t) {
        for (Lit l : assumeLits)
          stack.push_back({nodeOf(l), t});
        if (!checkPrefix)
          continue;
        for (Lit l : assertLits)
          stack.push_back({nodeOf(l), t});
        for (Lit l : outLits)
          stack.push_back({nodeOf(l), t});
      }
      DenseSet<std::pair<unsigned, unsigned>> seen;
      while (!stack.empty()) {
        auto [n, t] = stack.pop_back_val();
        if (n == 0 || !seen.insert({n, t}).second)
          continue;
        if (t < i && frames[t][n] != TX)
          continue;
        if (net.kind[n] == Net::State) {
          auto &r = regs[regOfNode.at(n)];
          if (t > 0 && states[t][regOfNode.at(n)] == TX)
            stack.push_back({nodeOf(r.next), t - 1});
        } else if (net.kind[n] == Net::And) {
          ++gates;
          for (Lit l : net.fanins[n])
            stack.push_back({nodeOf(l), t});
        }
      }
      return gates;
    };
    int64_t best = int64_t(count(joins[0])) + constantGates(joins[0]);
    for (unsigned i = 1; i <= last; ++i) {
      int64_t value = int64_t(count(joins[i])) + constantGates(joins[i]) -
                      int64_t(prefixGates(i));
      LLVM_DEBUG(llvm::dbgs() << "temporal-decomposition: prefix " << i << ": "
                              << count(joins[i]) << " constant bits, "
                              << constantGates(joins[i]) << " constant gates, "
                              << prefixGates(i)
                              << " prefix gates (no prefix: " << best << ")\n");
      if (value > best) {
        best = value;
        k = i;
      }
    }
  }
  auto &join = joins[k];
  if (!count(join))
    return res;
  res.constant = count(joins[0]);
  res.transient = count(join) - res.constant;
  res.prefix = k;
  res.changed = true;
  ArrayRef<uint8_t> at = states[k];
  LLVM_DEBUG(llvm::dbgs() << "temporal-decomposition: prefix " << k << ", "
                          << res.constant << " constant and " << res.transient
                          << " transient register bits of " << regs.size()
                          << "\n");

  unsigned base = 0;
  if (auto old = module->getAttrOfType<IntegerAttr>("verif.target_shift"))
    base = old.getInt();
  Value clock = regs[0].op.getClk();
  OpBuilder builder(outputOp);
  auto loc = module.getLoc();
  SigBuilder sb(builder, loc);
  Value initials[2];
  auto initialValue = [&](bool bit) {
    auto &v = initials[bit];
    if (!v) {
      OpBuilder::InsertionGuard guard(builder);
      builder.setInsertionPointToStart(body);
      v = seq::createConstantInitialValue(
          builder, loc, builder.getIntegerAttr(builder.getI1Type(), bit));
    }
    return v;
  };
  // first: 1 in the first cycle only.
  Value firstReg;
  auto first = [&]() -> Sig {
    if (!firstReg)
      firstReg = seq::CompRegOp::create(
          builder, loc, sb.value(SigBuilder::constant(false)), clock, Value(),
          Value(), builder.getStringAttr("prefix_first"), hw::InnerSymAttr(),
          initialValue(true));
    return {firstReg, false};
  };

  // The value of a node in cycle t of the prefix (t < k), or of a state bit
  // in cycle k, as logic over new inputs.
  DenseMap<std::pair<unsigned, unsigned>, Sig> unrolled;
  DenseMap<std::pair<unsigned, unsigned>, Value> prefixInputs;
  // New inputs are placeholders until all are known (adding ports one by
  // one is quadratic).
  SmallVector<std::pair<hw::ConstantOp, std::string>> newInputs;
  auto newInput = [&](const Twine &name, Type type) -> Value {
    auto placeholder = hw::ConstantOp::create(
        builder, loc, APInt(cast<IntegerType>(type).getWidth(), 0));
    newInputs.push_back({placeholder, name.str()});
    ++res.inputs;
    return placeholder;
  };
  auto unroll = [&](Lit root, unsigned frame) -> Sig {
    SmallVector<std::pair<unsigned, unsigned>> stack{{nodeOf(root), frame}};
    while (!stack.empty()) {
      auto key = stack.pop_back_val();
      auto [n, t] = key;
      if (unrolled.count(key))
        continue;
      uint8_t known = TX;
      if (n == 0)
        known = TZero;
      else if (t < k)
        known = frames[t][n];
      else if (net.kind[n] == Net::State)
        known = at[regOfNode.at(n)];
      if (known != TX) {
        unrolled[key] = SigBuilder::constant(known == TOne);
        continue;
      }
      switch (net.kind[n]) {
      case Net::Const:
        break;
      case Net::Free: {
        auto [value, bit] = leafOrigin.at(n);
        auto arg = cast<BlockArgument>(value);
        auto &input = prefixInputs[{arg.getArgNumber(), t}];
        if (!input)
          input = newInput(Twine(module.getInputName(arg.getArgNumber())) +
                               "_pre" + Twine(base + t),
                           arg.getType());
        Value v = input;
        if (!arg.getType().isInteger(1))
          v = comb::ExtractOp::create(builder, loc, v, bit, 1);
        unrolled[key] = {v, false};
        break;
      }
      case Net::State: {
        auto &r = regs[regOfNode.at(n)];
        if (t == 0) {
          // No initial value: any value.
          Value v =
              newInput("init" + Twine(regOfNode.at(n)) + "_pre" + Twine(base),
                       builder.getI1Type());
          unrolled[key] = {v, false};
          break;
        }
        auto it = unrolled.find({nodeOf(r.next), t - 1});
        if (it == unrolled.end()) {
          stack.push_back(key);
          stack.push_back({nodeOf(r.next), t - 1});
          break;
        }
        Sig s = it->second;
        unrolled[key] = isNeg(r.next) ? ~s : s;
        break;
      }
      case Net::And: {
        bool ready = true;
        for (Lit l : net.fanins[n]) {
          if (unrolled.count({nodeOf(l), t}))
            continue;
          if (ready)
            stack.push_back(key);
          ready = false;
          stack.push_back({nodeOf(l), t});
        }
        if (!ready)
          break;
        SmallVector<Sig> ins;
        for (Lit l : net.fanins[n]) {
          Sig s = unrolled.lookup({nodeOf(l), t});
          ins.push_back(isNeg(l) ? ~s : s);
        }
        unrolled[key] = sb.land(ins);
        break;
      }
      }
    }
    Sig s = unrolled.lookup({nodeOf(root), frame});
    return isNeg(root) ? ~s : s;
  };

  if (k > 0) {
    // G_t: the constraints hold in the cycles 0..t.
    SmallVector<Sig> guards;
    Sig guard = SigBuilder::constant(true);
    for (unsigned t = 0; t < k; ++t) {
      SmallVector<Sig> all{guard};
      for (Lit l : assumeLits)
        all.push_back(unroll(l, t));
      guard = sb.land(all);
      guards.push_back(guard);
    }
    // m: the first cycle after a prefix that breaks a constraint.
    Sig invalid = SigBuilder::constant(false);
    if (!guard.isTrue())
      invalid = sb.land({first(), ~guard});
    // first & (the property fails in the prefix).
    auto prefixFail = [&](Lit hold) -> Sig {
      if (!checkPrefix)
        return SigBuilder::constant(false);
      SmallVector<Sig> fails;
      for (unsigned t = 0; t < k; ++t)
        fails.push_back(sb.land({guards[t], ~unroll(hold, t)}));
      Sig fail = sb.lor(fails);
      if (fail.isFalse())
        return fail;
      return sb.land({first(), fail});
    };
    auto holdNow = [&](Value prop, Value en) -> Sig {
      if (!en)
        return {prop, false};
      return ~sb.land({Sig{en, false}, Sig{prop, true}});
    };
    for (auto [a, l] : llvm::zip(asserts, assertLits)) {
      Sig fail = prefixFail(l);
      if (fail.isFalse() && invalid.isFalse())
        continue;
      Sig hold = sb.land(
          {~fail, sb.lor({holdNow(a.getProperty(), a.getEnable()), invalid})});
      a.getPropertyMutable().assign(sb.value(hold));
      a.getEnableMutable().clear();
    }
    for (auto [i, l] : llvm::enumerate(outLits)) {
      Sig fail = prefixFail(l ^ 1);
      if (fail.isFalse() && invalid.isFalse())
        continue;
      Sig bad = sb.lor(
          {fail, sb.land({Sig{outputOp->getOperand(i), false}, ~invalid})});
      outputOp->setOperand(i, sb.value(bad));
    }
    if (!invalid.isFalse()) {
      for (auto a : assumes) {
        Sig hold = sb.lor({holdNow(a.getProperty(), a.getEnable()), invalid});
        a.getPropertyMutable().assign(sb.value(hold));
        a.getEnableMutable().clear();
      }
      Value ended = seq::CompRegOp::create(
          builder, loc, sb.value(invalid), clock, Value(), Value(),
          builder.getStringAttr("prefix_invalid"), hw::InnerSymAttr(),
          initialValue(false));
      verif::AssumeOp::create(builder, loc, sb.value(Sig{ended, true}), Value(),
                              StringAttr());
    }
  }

  // Rebuild the registers, one per bit that is not constant.
  llvm::MapVector<Operation *, SmallVector<unsigned>> bitsOf;
  for (unsigned i = 0; i < regs.size(); ++i)
    bitsOf[regs[i].op].push_back(i);
  for (auto &[opPtr, indices] : bitsOf) {
    auto op = cast<seq::CompRegOp>(opPtr);
    if (k == 0 &&
        llvm::none_of(indices, [&](unsigned i) { return join[i] != TX; }))
      continue;
    OpBuilder regBuilder(op);
    auto regLoc = op.getLoc();
    SmallVector<Value> bits;
    for (auto [bit, i] : llvm::enumerate(indices)) {
      if (join[i] != TX) {
        bits.push_back(sb.value(SigBuilder::constant(join[i] == TOne)));
        continue;
      }
      Value next = op.getInput();
      if (indices.size() > 1)
        next = comb::ExtractOp::create(regBuilder, regLoc, next, bit, 1);
      // The initial value: the one of cycle k if it is known.
      std::optional<bool> init = regs[i].init;
      if (k > 0) {
        init.reset();
        if (at[i] != TX)
          init = at[i] == TOne;
      }
      StringAttr name = op.getNameAttr();
      if (name && indices.size() > 1)
        name =
            regBuilder.getStringAttr(name.getValue() + "[" + Twine(bit) + "]");
      Value reg = seq::CompRegOp::create(
          regBuilder, regLoc, next, op.getClk(), Value(), Value(), name,
          hw::InnerSymAttr(), init ? initialValue(*init) : Value());
      if (k > 0 && at[i] == TX) {
        // It depends on the prefix: its logic in the first cycle.
        Sig start = unroll(mkLit(regs[i].node), k);
        Sig f = first();
        reg = sb.value(
            sb.lor({sb.land({f, start}), sb.land({~f, Sig{reg, false}})}));
      }
      bits.push_back(reg);
    }
    Value whole = bits[0];
    if (bits.size() > 1) {
      SmallVector<Value> msbFirst(llvm::reverse(bits));
      whole = comb::ConcatOp::create(regBuilder, regLoc, msbFirst);
    }
    op.getResult().replaceAllUsesWith(whole);
    op.erase();
  }
  // Add the new inputs.
  if (!newInputs.empty()) {
    Namespace names;
    for (auto port : module.getPortList())
      names.newName(port.name.getValue());
    SmallVector<std::pair<unsigned, hw::PortInfo>> ports;
    unsigned index = module.getNumInputPorts();
    for (auto &[placeholder, name] : newInputs) {
      hw::PortInfo port;
      port.name = builder.getStringAttr(names.newName(name));
      port.dir = hw::ModulePort::Direction::Input;
      port.type = placeholder.getType();
      ports.push_back({index, port});
    }
    // The block arguments first: updating the ports sets their locations.
    for (auto &[placeholder, name] : newInputs) {
      Value arg = body->addArgument(placeholder.getType(), loc);
      placeholder.getResult().replaceAllUsesWith(arg);
      placeholder.erase();
    }
    module.modifyPorts(ports, {}, {}, {});
  }
  if (k > 0)
    module->setAttr(
        "verif.target_shift",
        IntegerAttr::get(IntegerType::get(module.getContext(), 32), base + k));
  return res;
}

std::optional<Correspondence::Conflict>
Correspondence::checkConstraints(unsigned depth, bool &gaveUp) {
  gaveUp = false;
  // The constraints and their literals: `enable -> property`.
  SmallVector<verif::AssumeOp> assumes;
  SmallVector<Lit> lits;
  for (auto op : module.getBodyBlock()->getOps<verif::AssumeOp>()) {
    if (!op.getProperty().getType().isInteger(1))
      continue;
    Lit l = lit(op.getProperty(), 0);
    if (auto en = op.getEnable()) {
      Lit e = lit(en, 0);
      if (e == kFalse)
        continue;
      if (e != (kFalse ^ 1))
        l = mkLit(net.add(Net::And, {e, l ^ 1})) ^ 1;
    }
    if (l == (kFalse ^ 1))
      continue;
    assumes.push_back(op);
    lits.push_back(l);
  }
  if (assumes.empty())
    return std::nullopt;

  auto solver = createSATSolver(opts.satSolver);
  if (!solver) {
    gaveUp = true;
    return std::nullopt;
  }
  solver->setConflictLimit(opts.conflictLimit);
  int trueVar = solver->newVar();
  solver->addClause({trueVar});
  // One selector per constraint, shared by all frames.
  SmallVector<int> sel;
  for (size_t i = 0; i < assumes.size(); ++i)
    sel.push_back(solver->newVar());

  // Frames from the initial state: registers with an initial value start
  // there, the other leaves are free in every frame. A free leaf only adds
  // behaviors, so a conflict found here is one of the module.
  static const DenseMap<unsigned, Subst> noSubst;
  SmallVector<std::unique_ptr<FrameEncoder>> frames;
  for (unsigned f = 0; f < depth; ++f) {
    auto leaf = [&, f](unsigned n) -> int {
      if (net.kind[n] == Net::State) {
        auto &r = regs[regOfNode.at(n)];
        if (f > 0)
          return (*frames[f - 1])(r.next);
        if (r.init)
          return *r.init ? trueVar : -trueVar;
      }
      return solver->newVar();
    };
    frames.push_back(
        std::make_unique<FrameEncoder>(*solver, net, trueVar, leaf, noSubst));
    for (auto [s, l] : llvm::zip(sel, lits))
      solver->addClause({-s, (*frames[f])(l)});
    ++numSATCalls;
    auto result = solver->solve(sel);
    if (result == IncrementalSATSolver::kSAT)
      continue;
    if (result != IncrementalSATSolver::kUNSAT) {
      ++numUnknown;
      gaveUp = true;
      return std::nullopt;
    }
    // A conflict within f + 1 cycles. Shrink the constraints to a core by
    // deletion: drop each one whose removal keeps the conflict.
    SmallVector<bool> inCore(assumes.size(), true);
    for (size_t i = 0; i < assumes.size(); ++i) {
      SmallVector<int> active;
      for (size_t j = 0; j < assumes.size(); ++j)
        if (j != i && inCore[j])
          active.push_back(sel[j]);
      ++numSATCalls;
      if (solver->solve(active) == IncrementalSATSolver::kUNSAT)
        inCore[i] = false;
    }
    Conflict c;
    c.cycles = f + 1;
    for (size_t i = 0; i < assumes.size(); ++i)
      if (inCore[i])
        c.core.push_back(assumes[i]);
    return c;
  }
  return std::nullopt;
}

std::pair<unsigned, unsigned> Correspondence::merge() {
  // Split the registers involved into one register per bit.
  llvm::SetVector<Operation *> involved;
  for (auto &c : classes)
    for (auto [i, p] : c.members)
      if (cands[i].reg >= 0)
        involved.insert(regs[cands[i].reg].op);
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
    // Modeled registers have a constant initial value or none.
    std::optional<APInt> init;
    (void)getConstantInit(op, init);
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
  // The value of a candidate after the split.
  auto candValue = [&](unsigned i) -> Value {
    if (cands[i].reg >= 0) {
      auto &r = regs[cands[i].reg];
      return bitValue.at({r.op, r.bit});
    }
    return cands[i].value;
  };
  // Does the combinational cone of `root` in the IR contain `target`?
  auto reaches = [&](Value root, Value target) {
    SmallVector<Value> stack{root};
    DenseSet<Value> seen;
    while (!stack.empty()) {
      Value v = stack.pop_back_val();
      if (v == target)
        return true;
      if (!seen.insert(v).second)
        continue;
      Operation *op = v.getDefiningOp();
      if (!op || isa<seq::CompRegOp, seq::InitialOp>(op))
        continue;
      for (auto operand : op->getOperands())
        stack.push_back(operand);
    }
    return false;
  };
  // Replace the members by the class value.
  unsigned mergedRegs = 0, mergedGates = 0;
  OpBuilder builder(module.getContext());
  builder.setInsertionPoint(module.getBodyBlock()->getTerminator());
  for (auto &c : classes) {
    Value ref;
    if (!c.constant)
      ref = candValue(c.members[0].first);
    Value inverse, constant[2];
    for (auto [i, p] : c.members) {
      Value v = candValue(i);
      if (v == ref)
        continue;
      auto loc = v.getLoc();
      Value repl;
      if (c.constant) {
        if (!constant[p])
          constant[p] = hw::ConstantOp::create(
              builder, loc, builder.getIntegerAttr(builder.getI1Type(), p));
        repl = constant[p];
      } else if (p) {
        if (!inverse)
          inverse = aig::AndInverterOp::create(builder, loc, ref, true);
        repl = inverse;
      } else {
        repl = ref;
      }
      if (cands[i].reg >= 0) {
        auto reg = v.getDefiningOp<seq::CompRegOp>();
        reg.getResult().replaceAllUsesWith(repl);
        reg.erase();
        ++mergedRegs;
        continue;
      }
      // An and-gate: its representative comes earlier in the net, so it does
      // not depend on it there; an op outside the net could still make it
      // depend on it in the module.
      if (hiddenPaths && ref && reaches(ref, v))
        continue;
      v.replaceAllUsesWith(repl);
      ++mergedGates;
    }
  }
  return {mergedRegs, mergedGates};
}

namespace {
struct RegisterCorrespondencePass
    : public circt::synth::impl::RegisterCorrespondenceBase<
          RegisterCorrespondencePass> {
  using RegisterCorrespondenceBase::RegisterCorrespondenceBase;

  void runOnOperation() override {
    auto module = getOperation();
    Params opts{numPatterns,   numFrames,          seed,
                maxIterations, int(conflictLimit), satSolver,
                internal,      cexFrames,          chunk};
    Correspondence c(module, opts);
    if (!c.build())
      return markAllAnalysesPreserved();
    c.simulateCandidates();
    numCandidatesStat += c.numCandidates();
    bool proved = chunk ? c.proveIncremental(false) && c.proveIncremental(true)
                        : c.proveBase() && c.proveStep();
    numIterationsStat += c.numIterations;
    numSATCallsStat += c.numSATCalls;
    numUnknownStat += c.numUnknown;
    if (!proved) {
      module.emitWarning("register correspondence gave up after ")
          << c.numIterations << " iterations; nothing merged";
      return markAllAnalysesPreserved();
    }
    auto [regs, gates] = c.merge();
    numMergedStat += regs;
    numMergedGatesStat += gates;
  }
};
} // namespace

namespace {
struct TernaryConstantsPass
    : public circt::synth::impl::TernaryConstantsBase<TernaryConstantsPass> {
  void runOnOperation() override {
    Params opts{64, 1, 0, 0, -1, "auto", false, 0, 0};
    Correspondence c(getOperation(), opts);
    if (!c.build() || !c.ternaryConstants())
      return markAllAnalysesPreserved();
    numConstantStat += c.merge().first;
  }
};
} // namespace

namespace {
struct CheckConstraintsPass
    : public circt::synth::impl::CheckConstraintsBase<CheckConstraintsPass> {
  using CheckConstraintsBase::CheckConstraintsBase;

  void runOnOperation() override {
    auto module = getOperation();
    markAllAnalysesPreserved();
    Params opts{64, 1, 0, 0, int(conflictLimit), satSolver, false, 0, 0};
    Correspondence c(module, opts);
    c.build();
    bool gaveUp;
    auto conflict = c.checkConstraints(depth, gaveUp);
    numSATCallsStat += c.numSATCalls;
    if (gaveUp)
      module.emitWarning("constraint check gave up (conflict limit)");
    if (!conflict)
      return;
    ++numConflictsStat;
    auto diag = failOnConflict ? module.emitError() : module.emitWarning();
    if (conflict->cycles == 1)
      diag << "constraints conflict in the initial state; every property "
              "holds vacuously";
    else
      diag << "constraints conflict within " << conflict->cycles
           << " cycles; no trace is longer than " << conflict->cycles - 1
           << " cycles";
    for (auto op : conflict->core)
      diag.attachNote(op.getLoc()) << "constraint in the conflict";
    if (failOnConflict)
      signalPassFailure();
  }
};
} // namespace

namespace {
struct TemporalDecompositionPass
    : public circt::synth::impl::TemporalDecompositionBase<
          TemporalDecompositionPass> {
  using TemporalDecompositionBase::TemporalDecompositionBase;

  void runOnOperation() override {
    Params opts{64, 1, 0, 0, -1, "auto", false, 0, 0};
    Correspondence c(getOperation(), opts);
    if (!c.build())
      return markAllAnalysesPreserved();
    auto res = c.decompose(maxPrefix, checkPrefix, badOutputs, force);
    if (!res.changed)
      return markAllAnalysesPreserved();
    prefixStat += res.prefix;
    numConstantStat += res.constant;
    numTransientStat += res.transient;
    numInputsStat += res.inputs;
  }
};
} // namespace
