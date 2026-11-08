//===- RemoveFalseCycles.cpp - Remove functionally acyclic cycles ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A combinational cycle may still compute a well-defined value. Example, from
// a processor: `valid = in && !kill`, `kill = step && valid_out`,
// `valid_out = valid && !halt`, where the control logic never lets the cycle
// be sensitized. Passes that need a topological order reject such modules.
//
// This pass rebuilds the ops of each cycle bit by bit, which removes cycles
// that exist only at word level. For a cycle of bits, it picks cut bits X such
// that the rest of the cycle is acyclic, which makes the cut bits a function
// X' = F(X, inputs) of themselves. If F^k does not depend on X (checked with a
// SAT solver), the cycle has exactly one fixed point,
//
//   X = F^k(X) = F^k(0),
//
// and the cut bits are replaced by k acyclic copies of the cycle computing
// F^k(0). The result is equivalent to the original module.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Synth/SynthOps.h"
#include "circt/Dialect/Synth/Transforms/SynthPasses.h"
#include "circt/Support/SATSolver.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Matchers.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "synth-remove-false-cycles"

using namespace circt;
using namespace circt::synth;
using namespace mlir;

namespace circt {
namespace synth {
#define GEN_PASS_DEF_REMOVEFALSECYCLES
#include "circt/Dialect/Synth/Transforms/SynthPasses.h.inc"
} // namespace synth
} // namespace circt

namespace {
struct RemoveFalseCyclesPass
    : public synth::impl::RemoveFalseCyclesBase<RemoveFalseCyclesPass> {
  using RemoveFalseCyclesBase::RemoveFalseCyclesBase;
  void runOnOperation() override;
};

using Adjacency = SmallVector<SmallVector<unsigned, 4>>;

/// Strongly connected components (Tarjan, iterative). Returns their number
/// and sets the component of each node.
unsigned computeSCCs(const Adjacency &adj, SmallVectorImpl<unsigned> &comp) {
  unsigned n = adj.size(), next = 0, numComps = 0;
  SmallVector<unsigned> index(n, ~0u), low(n), stack;
  SmallVector<bool> onStack(n);
  SmallVector<std::pair<unsigned, unsigned>> work;
  comp.assign(n, ~0u);
  auto enter = [&](unsigned v) {
    index[v] = low[v] = next++;
    stack.push_back(v);
    onStack[v] = true;
    work.push_back({v, 0});
  };
  for (unsigned root = 0; root < n; ++root) {
    if (index[root] != ~0u)
      continue;
    enter(root);
    while (!work.empty()) {
      unsigned v = work.back().first;
      if (work.back().second < adj[v].size()) {
        unsigned w = adj[v][work.back().second++];
        if (index[w] == ~0u)
          enter(w);
        else if (onStack[w])
          low[v] = std::min(low[v], index[w]);
        continue;
      }
      work.pop_back();
      if (!work.empty())
        low[work.back().first] = std::min(low[work.back().first], low[v]);
      if (low[v] != index[v])
        continue;
      unsigned w;
      do {
        w = stack.pop_back_val();
        onStack[w] = false;
        comp[w] = numComps;
      } while (w != v);
      ++numComps;
    }
  }
  return numComps;
}

/// The components that contain a cycle.
SmallVector<SmallVector<unsigned>> cyclicSCCs(const Adjacency &adj) {
  SmallVector<unsigned> comp;
  unsigned numComps = computeSCCs(adj, comp);
  SmallVector<SmallVector<unsigned>> members(numComps);
  for (unsigned v = 0; v < adj.size(); ++v)
    members[comp[v]].push_back(v);
  SmallVector<SmallVector<unsigned>> result;
  for (auto &m : members)
    if (m.size() > 1 || llvm::is_contained(adj[m[0]], m[0]))
      result.push_back(std::move(m));
  return result;
}

unsigned getWidth(Value value) {
  return cast<IntegerType>(value.getType()).getWidth();
}

bool isWire(Operation *op) {
  return isa<comb::ExtractOp, comb::ConcatOp, comb::ReplicateOp>(op);
}

/// Follow `bit` of `value` through extract, concat and replicate ops (all of
/// them if `ops` is null, else those in `ops`).
std::pair<Value, unsigned> throughWires(Value value, unsigned bit,
                                        const DenseSet<Operation *> *ops) {
  while (auto *op = value.getDefiningOp()) {
    if (!isWire(op) || (ops && !ops->contains(op)))
      break;
    if (auto extract = dyn_cast<comb::ExtractOp>(op)) {
      bit += extract.getLowBit();
      value = extract.getInput();
    } else if (auto replicate = dyn_cast<comb::ReplicateOp>(op)) {
      bit %= getWidth(replicate.getInput());
      value = replicate.getInput();
    } else {
      unsigned pos = 0;
      for (auto input : llvm::reverse(cast<comb::ConcatOp>(op).getInputs())) {
        unsigned width = getWidth(input);
        if (bit < pos + width) {
          value = input;
          bit -= pos;
          break;
        }
        pos += width;
      }
    }
  }
  return {value, bit};
}

/// A bit read by a node: a constant, a bit of a value outside the cycle, or
/// another node.
struct BitRef {
  enum Kind { Const, Outside, Node } kind;
  bool constant = false;
  Value value;
  unsigned bit = 0;
  unsigned node = 0;
};

/// One bit of an and-inverter op of a cycle of ops.
struct Node {
  aig::AndInverterOp op;
  unsigned bit;
  SmallVector<std::pair<BitRef, bool>, 4> inputs;
  Value final;
};

/// A cycle of bits, its cut bits and the number of applications of the
/// cycle logic after which it no longer depends on them.
struct BitCycle {
  SmallVector<unsigned> order; // operands first, ignoring edges to cuts
  SmallVector<unsigned> cuts;
  DenseSet<unsigned> members, isCut;
  unsigned unroll = 0;
  /// Rebuilt without a proof: the module is equivalent only if the
  /// obligation output is 0 in every reachable state.
  bool obligation = false;
};

/// A cycle of ops, rebuilt bit by bit.
struct OpCycle {
  DenseSet<Operation *> ops;
  SmallVector<Operation *> opList;
  SmallVector<Node> nodes;
  DenseMap<std::pair<Operation *, unsigned>, unsigned> nodeIds;
  SmallVector<BitCycle> bitCycles;
  SmallVector<unsigned> bitCycleOf; // per node, ~0u if not in a bit cycle

  BitRef resolve(Value value, unsigned bit) const {
    std::tie(value, bit) = throughWires(value, bit, &ops);
    if (auto *op = value.getDefiningOp(); op && ops.contains(op))
      return {BitRef::Node, false, {}, 0, nodeIds.at({op, bit})};
    APInt c;
    if (matchPattern(value, m_ConstantInt(&c)))
      return {BitRef::Const, c[bit], {}, 0, 0};
    return {BitRef::Outside, false, value, bit, 0};
  }
};

/// SAT encoding of bits of the module, outside of the cycles being checked.
class Encoder {
public:
  using Key = std::pair<Value, unsigned>;
  Encoder(IncrementalSATSolver &solver, const DenseSet<Key> &cycleBits,
          unsigned budget)
      : solver(solver), cycleBits(cycleBits), budget(budget) {
    trueLit = solver.newVar();
    solver.addClause({trueLit});
  }

  int constant(bool value) const { return value ? trueLit : -trueLit; }
  int newVar() { return solver.newVar(); }
  void addAnd(int out, ArrayRef<int> lits) {
    addAndClauses(out, lits,
                  [&](ArrayRef<int> clause) { solver.addClause(clause); });
  }

  /// The literal of `bit` of `value`. Bits in cycles, of unknown ops, or
  /// beyond the budget are free: this can only make the check fail.
  int encode(Value value, unsigned bit) {
    SmallVector<std::pair<Key, bool>> stack{
        {throughWires(value, bit, {}), false}};
    DenseSet<Key> inProgress;
    while (!stack.empty()) {
      auto [key, expanded] = stack.pop_back_val();
      if (lits.count(key))
        continue;
      auto [v, b] = key;
      APInt c;
      if (matchPattern(v, m_ConstantInt(&c))) {
        lits[key] = constant(c[b]);
        continue;
      }
      auto andOp = v.getDefiningOp<aig::AndInverterOp>();
      if (!andOp || cycleBits.contains(key) || numEncoded >= budget ||
          (!expanded && inProgress.contains(key))) {
        lits[key] = solver.newVar();
        freeBits.push_back({key, lits[key]});
        continue;
      }
      if (!expanded) {
        inProgress.insert(key);
        stack.push_back({key, true});
        for (auto input : andOp.getInputs())
          stack.push_back({throughWires(input, b, {}), false});
        continue;
      }
      SmallVector<int> inputs;
      for (auto [input, inverted] :
           llvm::zip(andOp.getInputs(), andOp.getInverted())) {
        int lit = lits.at(throughWires(input, b, {}));
        inputs.push_back(inverted ? -lit : lit);
      }
      int out = solver.newVar();
      addAnd(out, inputs);
      lits[key] = out;
      ++numEncoded;
    }
    return lits.at(throughWires(value, bit, {}));
  }

private:
  IncrementalSATSolver &solver;
  const DenseSet<Key> &cycleBits;
  unsigned budget;
  unsigned numEncoded = 0;

public:
  /// Bits encoded as free variables (for debugging counterexamples).
  SmallVector<std::pair<Key, int>> freeBits;
  unsigned getNumEncoded() const { return numEncoded; }

private:
  int trueLit;
  DenseMap<Key, int> lits;
};
} // namespace

/// Choose cut bits for a cycle of bits: the targets of the back edges of a
/// depth-first search, so that the cycle is acyclic without the edges into
/// them. Also orders the bits, operands first.
static void chooseCuts(OpCycle &cycle, BitCycle &bc) {
  DenseMap<unsigned, unsigned> state; // 1: on stack, 2: done
  SmallVector<std::pair<unsigned, unsigned>> work;
  for (unsigned root : bc.members) {
    if (state.count(root))
      continue;
    state[root] = 1;
    work.push_back({root, 0});
    while (!work.empty()) {
      auto [v, i] = work.back();
      auto &inputs = cycle.nodes[v].inputs;
      if (i < inputs.size()) {
        ++work.back().second;
        auto &ref = inputs[i].first;
        if (ref.kind != BitRef::Node || !bc.members.contains(ref.node) ||
            bc.isCut.contains(ref.node))
          continue;
        auto it = state.find(ref.node);
        if (it == state.end()) {
          state[ref.node] = 1;
          work.push_back({ref.node, 0});
        } else if (it->second == 1) {
          bc.isCut.insert(ref.node);
          bc.cuts.push_back(ref.node);
        }
        continue;
      }
      work.pop_back();
      state[v] = 2;
      bc.order.push_back(v);
    }
  }
}

/// Encode one application of the logic of `bc`, with the cut bits taking
/// the values `cuts`. Returns the new values of the cut bits.
static SmallVector<int> encodeStep(OpCycle &cycle, BitCycle &bc,
                                   Encoder &encoder, ArrayRef<int> cuts) {
  DenseMap<unsigned, int> lits;
  for (auto [cut, lit] : llvm::zip(bc.cuts, cuts))
    lits[cut] = lit;
  DenseMap<unsigned, int> step;
  for (unsigned n : bc.order) {
    SmallVector<int> inputs;
    for (auto &[ref, inverted] : cycle.nodes[n].inputs) {
      int lit;
      if (ref.kind == BitRef::Const)
        lit = encoder.constant(ref.constant);
      else if (ref.kind == BitRef::Outside)
        lit = encoder.encode(ref.value, ref.bit);
      else if (bc.isCut.contains(ref.node))
        lit = lits.at(ref.node);
      else if (bc.members.contains(ref.node))
        lit = step.at(ref.node);
      else
        lit = encoder.encode(cycle.nodes[ref.node].op.getResult(),
                             cycle.nodes[ref.node].bit);
      inputs.push_back(inverted ? -lit : lit);
    }
    int out = encoder.newVar();
    encoder.addAnd(out, inputs);
    step[n] = out;
  }
  SmallVector<int> result;
  for (unsigned cut : bc.cuts)
    result.push_back(step.at(cut));
  return result;
}

void RemoveFalseCyclesPass::runOnOperation() {
  auto module = getOperation();
  Block &block = *module.getBodyBlock();

  // Cycles of ops.
  auto isLogic = [&](Operation *op) {
    return op && op->getBlock() == &block && op->getNumResults() == 1 &&
           isa<IntegerType>(op->getResult(0).getType()) &&
           (isa<aig::AndInverterOp>(op) || isWire(op));
  };
  SmallVector<Operation *> ops;
  DenseMap<Operation *, unsigned> opIndex;
  for (auto &op : block)
    if (isLogic(&op)) {
      opIndex[&op] = ops.size();
      ops.push_back(&op);
    }
  Adjacency opAdj(ops.size());
  for (auto [i, op] : llvm::enumerate(ops))
    for (auto operand : op->getOperands())
      if (auto *def = operand.getDefiningOp(); isLogic(def))
        opAdj[i].push_back(opIndex.at(def));
  auto opSCCs = cyclicSCCs(opAdj);
  if (opSCCs.empty())
    return;

  // Bits of each cycle of ops and the cycles of bits among them.
  SmallVector<OpCycle, 0> cycles(opSCCs.size());
  DenseSet<Encoder::Key> cycleBits;
  for (auto [scc, cycle] : llvm::zip(opSCCs, cycles)) {
    for (unsigned i : scc) {
      cycle.ops.insert(ops[i]);
      cycle.opList.push_back(ops[i]);
    }
    for (auto *op : cycle.opList)
      if (auto andOp = dyn_cast<aig::AndInverterOp>(op))
        for (unsigned b = 0, e = getWidth(andOp.getResult()); b < e; ++b) {
          cycle.nodeIds[{op, b}] = cycle.nodes.size();
          cycle.nodes.push_back({andOp, b, {}, {}});
        }
    Adjacency bitAdj(cycle.nodes.size());
    for (auto [n, node] : llvm::enumerate(cycle.nodes)) {
      for (auto [input, inverted] :
           llvm::zip(node.op.getInputs(), node.op.getInverted())) {
        auto ref = cycle.resolve(input, node.bit);
        node.inputs.push_back({ref, inverted});
        if (ref.kind == BitRef::Node)
          bitAdj[n].push_back(ref.node);
      }
    }
    cycle.bitCycleOf.assign(cycle.nodes.size(), ~0u);
    for (auto &members : cyclicSCCs(bitAdj)) {
      BitCycle bc;
      for (unsigned n : members) {
        bc.members.insert(n);
        cycle.bitCycleOf[n] = cycle.bitCycles.size();
        cycleBits.insert({cycle.nodes[n].op.getResult(), cycle.nodes[n].bit});
      }
      cycle.bitCycles.push_back(std::move(bc));
    }
  }

  // Check each cycle of bits: F^k(X) == F^k(Y) for all X, Y and inputs.
  std::unique_ptr<IncrementalSATSolver> solver;
  bool anyBitCycle = llvm::any_of(
      cycles, [](auto &cycle) { return !cycle.bitCycles.empty(); });
  if (anyBitCycle) {
    solver = createSATSolver(satSolver);
    if (!solver) {
      module.emitError() << "unsupported or unavailable SAT solver '"
                         << satSolver << "'";
      return signalPassFailure();
    }
    solver->setConflictLimit(static_cast<int>(conflictLimit));
  }
  std::optional<Encoder> encoder;
  if (solver)
    encoder.emplace(*solver, cycleBits, maxConeBits);
  SmallVector<bool> proved(cycles.size(), true);
  for (auto [i, cycle] : llvm::enumerate(cycles)) {
    for (auto &bc : cycle.bitCycles) {
      chooseCuts(cycle, bc);
      SmallVector<int> x, y;
      for (unsigned c = 0; c < bc.cuts.size(); ++c) {
        x.push_back(encoder->newVar());
        y.push_back(encoder->newVar());
      }
      bool unknown = false;
      for (unsigned k = 1; k <= maxUnroll && !bc.unroll && !unknown; ++k) {
        x = encodeStep(cycle, bc, *encoder, x);
        y = encodeStep(cycle, bc, *encoder, y);
        // d -> some cut bit differs.
        int d = encoder->newVar();
        SmallVector<int> clause{-d};
        for (auto [a, b] : llvm::zip(x, y)) {
          int diff = encoder->newVar();
          addXorClauses(diff, a, b,
                        [&](ArrayRef<int> cl) { solver->addClause(cl); });
          clause.push_back(diff);
        }
        solver->addClause(clause);
        auto result = solver->solve({d});
        if (result == IncrementalSATSolver::kUNSAT)
          bc.unroll = k;
        else if (result == IncrementalSATSolver::kSAT) {
          LLVM_DEBUG({
            llvm::dbgs() << "k = " << k << ": " << encoder->getNumEncoded()
                         << " bits encoded; free bits set:\n";
            for (auto [key, lit] : encoder->freeBits) {
              if (solver->val(lit) <= 0)
                continue;
              llvm::dbgs() << "  bit " << key.second << " of ";
              key.first.printAsOperand(llvm::dbgs(), OpPrintingFlags());
              if (auto *op = key.first.getDefiningOp())
                llvm::dbgs() << " " << op->getName() << " " << op->getLoc();
              llvm::dbgs() << "\n";
            }
          });
        } else
          unknown = true;
      }
      if (bc.unroll) {
        ++numBitCycles;
        numCuts += bc.cuts.size();
        continue;
      }
      ++numFailed;
      if (obligation &&
          (!fixpointObligation || bc.cuts.size() <= maxFixpointCuts)) {
        bc.obligation = true;
        bc.unroll = fixpointObligation ? 1 : unsigned(obligationUnroll);
        numCuts += bc.cuts.size();
      } else {
        proved[i] = false;
      }
      auto diag = module.emitWarning("combinational cycle of ")
                  << bc.members.size() << " bits "
                  << (unknown ? "could not be checked (SAT budget)"
                              : "depends on itself")
                  << (obligation ? "; rebuilt under the obligation"
                                 : "; left in place");
      unsigned shown = 0;
      for (unsigned n : bc.order)
        if (shown++ < 8)
          diag.attachNote(cycle.nodes[n].op.getLoc())
              << "bit " << cycle.nodes[n].bit << " is part of the cycle";
    }
  }

  // Rebuild the proved cycles of ops bit by bit.
  if (llvm::none_of(proved, [](bool p) { return p; }))
    return;
  OpBuilder builder(block.getTerminator());
  auto loc = module.getLoc();
  auto i1 = builder.getI1Type();
  Value constants[2] = {hw::ConstantOp::create(builder, loc, i1, 0),
                        hw::ConstantOp::create(builder, loc, i1, 1)};
  DenseMap<Encoder::Key, Value> outsideBits;
  SmallVector<Value> equalities;
  SmallVector<std::pair<Value, Value>> replacements;
  SmallVector<Operation *> erased;
  auto outside = [&](Value value, unsigned bit) -> Value {
    if (getWidth(value) == 1)
      return value;
    auto &result = outsideBits[{value, bit}];
    if (!result)
      result = comb::ExtractOp::create(builder, value.getLoc(), value, bit, 1);
    return result;
  };
  for (auto [i, cycleRef] : llvm::enumerate(cycles)) {
    if (!proved[i])
      continue;
    ++numWordCycles;
    OpCycle &cycle = cycleRef;
    auto bitValue = [&](const BitRef &ref) -> Value {
      if (ref.kind == BitRef::Const)
        return constants[ref.constant];
      if (ref.kind == BitRef::Outside)
        return outside(ref.value, ref.bit);
      return cycle.nodes[ref.node].final;
    };
    auto create = [&](Node &node, ArrayRef<Value> inputs) {
      return aig::AndInverterOp::create(builder, node.op.getLoc(), i1, inputs,
                                        node.op.getInverted())
          .getResult();
    };
    // The final bits, with their operands set below.
    for (auto &node : cycle.nodes) {
      SmallVector<Value> inputs(node.inputs.size(), constants[0]);
      node.final = create(node, inputs);
    }
    // k applications of the cycle logic to the cut bits `prev`.
    auto unrollCuts = [&](BitCycle &bc, DenseMap<unsigned, Value> &prev) {
      for (unsigned k = 0; k < bc.unroll; ++k) {
        DenseMap<unsigned, Value> step;
        for (unsigned n : bc.order) {
          SmallVector<Value> inputs;
          for (auto &[ref, inverted] : cycle.nodes[n].inputs) {
            if (ref.kind == BitRef::Node && bc.isCut.contains(ref.node))
              inputs.push_back(prev.at(ref.node));
            else if (ref.kind == BitRef::Node && bc.members.contains(ref.node))
              inputs.push_back(step.at(ref.node));
            else
              inputs.push_back(bitValue(ref));
          }
          step[n] = create(cycle.nodes[n], inputs);
        }
        for (unsigned cut : bc.cuts)
          prev[cut] = step.at(cut);
      }
    };
    auto andOf = [&](ArrayRef<Value> values, bool invertAll) -> Value {
      return aig::AndInverterOp::create(
                 builder, loc, i1, values,
                 SmallVector<bool>(values.size(), invertAll))
          .getResult();
    };
    auto notOf = [&](Value value) -> Value {
      return aig::AndInverterOp::create(builder, loc, value, /*invert=*/true);
    };
    DenseMap<unsigned, Value> cutValue;
    for (auto &bc : cycle.bitCycles) {
      if (bc.obligation && fixpointObligation) {
        // The cut bits are the fixed point among all 2^n values v of the
        // cut bits: F(v) == v. Obligation: exactly one v matches.
        SmallVector<Value> matches;
        unsigned n = bc.cuts.size();
        for (uint64_t v = 0; v < (uint64_t(1) << n); ++v) {
          DenseMap<unsigned, Value> prev;
          for (auto [j, cut] : llvm::enumerate(bc.cuts))
            prev[cut] = constants[(v >> j) & 1];
          unrollCuts(bc, prev);
          SmallVector<Value> literals;
          for (auto [j, cut] : llvm::enumerate(bc.cuts))
            literals.push_back((v >> j) & 1 ? prev.at(cut)
                                            : notOf(prev.at(cut)));
          matches.push_back(andOf(literals, false));
        }
        for (auto [j, cut] : llvm::enumerate(bc.cuts)) {
          SmallVector<Value> with;
          for (uint64_t v = 0; v < matches.size(); ++v)
            if ((v >> j) & 1)
              with.push_back(matches[v]);
          cutValue[cut] = notOf(andOf(with, true));
        }
        SmallVector<Value> ok{notOf(andOf(matches, true))};
        for (unsigned a = 0; a < matches.size(); ++a)
          for (unsigned b = a + 1; b < matches.size(); ++b)
            ok.push_back(notOf(andOf({matches[a], matches[b]}, false)));
        equalities.push_back(andOf(ok, false));
        ++numObligations;
        continue;
      }
      // The cut bits: F^k(0).
      DenseMap<unsigned, Value> prev;
      for (unsigned cut : bc.cuts)
        prev[cut] = constants[0];
      unrollCuts(bc, prev);
      for (unsigned cut : bc.cuts)
        cutValue[cut] = prev.at(cut);
      if (!bc.obligation)
        continue;
      // Obligation: F^k(Y1) == F^k(Y2) for cut bits Y1, Y2 given as new
      // module inputs.
      DenseMap<unsigned, Value> y[2];
      for (unsigned c = 0; c < 2; ++c) {
        auto name = builder.getStringAttr("false_cycle_y" + Twine(c + 1) + "_" +
                                          Twine(numObligations));
        auto arg =
            module.appendInput(name, builder.getIntegerType(bc.cuts.size()))
                .second;
        for (auto [j, cut] : llvm::enumerate(bc.cuts))
          y[c][cut] = outside(arg, j);
        unrollCuts(bc, y[c]);
      }
      for (unsigned cut : bc.cuts) {
        Value a = y[0].at(cut), b = y[1].at(cut);
        Value t1 = aig::AndInverterOp::create(builder, loc, a, b, false, true);
        Value t2 = aig::AndInverterOp::create(builder, loc, a, b, true, false);
        // 1 if the two agree.
        equalities.push_back(
            aig::AndInverterOp::create(builder, loc, t1, t2, true, true));
      }
      ++numObligations;
    }
    for (auto &node : cycle.nodes) {
      auto *op = node.final.getDefiningOp();
      for (auto [j, input] : llvm::enumerate(node.inputs)) {
        auto &ref = input.first;
        auto it =
            ref.kind == BitRef::Node ? cutValue.find(ref.node) : cutValue.end();
        op->setOperand(j, it != cutValue.end() ? it->second : bitValue(ref));
      }
    }
    // The new words. Replaced after all cycles are rebuilt: other cycles may
    // read these words as values outside of them.
    for (auto *op : cycle.opList) {
      Value result = op->getResult(0);
      SmallVector<Value> bits;
      for (unsigned b = getWidth(result); b-- > 0;)
        bits.push_back(bitValue(cycle.resolve(result, b)));
      replacements.push_back(
          {result, bits.size() == 1
                       ? bits[0]
                       : comb::ConcatOp::create(builder, op->getLoc(), bits)
                             .getResult()});
      erased.push_back(op);
    }
  }
  for (auto [from, to] : replacements)
    from.replaceAllUsesWith(to);
  for (auto *op : erased)
    op->dropAllReferences();
  for (auto *op : erased)
    op->erase();

  if (!equalities.empty()) {
    Value allEqual =
        equalities.size() == 1
            ? equalities[0]
            : aig::AndInverterOp::create(builder, loc, i1, equalities,
                                         SmallVector<bool>(equalities.size()))
                  .getResult();
    module.appendOutput("false_cycle_obligation",
                        aig::AndInverterOp::create(builder, loc, allEqual,
                                                   /*invert=*/true));
  }
}
