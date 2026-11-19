//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass implements FunctionalReduction (Functionally Reduced And-Inverter
// Graph) optimization. It identifies and merges functionally equivalent nodes
// through simulation-based candidate detection followed by SAT-based
// verification.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Synth/SynthOpInterfaces.h"
#include "circt/Dialect/Synth/SynthOps.h"
#include "circt/Dialect/Synth/Transforms/CutRewriter.h"
#include "circt/Dialect/Synth/Transforms/SynthPasses.h"
#include "circt/Support/SATSolver.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/Debug.h"
#include <chrono>
#include <functional>
#include <limits>
#include <random>

#define DEBUG_TYPE "synth-functional-reduction"

static constexpr llvm::StringLiteral kTestClassAttrName =
    "synth.test.fc_equiv_class";

namespace circt {
namespace synth {
#define GEN_PASS_DEF_FUNCTIONALREDUCTION
#include "circt/Dialect/Synth/Transforms/SynthPasses.h.inc"
} // namespace synth
} // namespace circt

using namespace circt;
using namespace circt::synth;

namespace {
enum class EquivResult { Proved, Disproved, Unknown };
using SATSolverFactory = std::function<std::unique_ptr<IncrementalSATSolver>()>;

static bool isFunctionalReductionSimulatableOp(Operation *op) {
  return isa<BooleanLogicOpInterface, comb::AndOp, comb::OrOp, comb::XorOp>(op);
}

/// Ops whose operands must be defined before them in the sorted block: the
/// logic network and the simulated comb ops.
static bool isFunctionalReductionNetworkOp(Operation *op) {
  return isLogicNetworkOp(op) || isFunctionalReductionSimulatableOp(op);
}

//===----------------------------------------------------------------------===//
// Core Functional Reduction Implementation
//===----------------------------------------------------------------------===//

class FunctionalReductionSolver {
public:
  FunctionalReductionSolver(hw::HWModuleOp module, unsigned numPatterns,
                            unsigned seed, bool testTransformation,
                            SATSolverFactory createSolver,
                            bool useChoices = true, bool refine = true,
                            unsigned maxSolverVars = 0,
                            bool orderFromOutputs = false)
      : module(module), numPatterns(numPatterns), seed(seed),
        testTransformation(testTransformation), useChoices(useChoices),
        refine(refine), maxSolverVars(maxSolverVars),
        orderFromOutputs(orderFromOutputs),
        createSolver(std::move(createSolver)) {}

  ~FunctionalReductionSolver() = default;

  /// Run the Functional Reduction algorithm and return statistics.
  struct Stats {
    unsigned numEquivClasses = 0;
    unsigned numProvedEquiv = 0;
    unsigned numDisprovedEquiv = 0;
    unsigned numUnknown = 0;
    unsigned numMergedNodes = 0;
    unsigned numSimDisproved = 0;
    unsigned numRounds = 0;
    unsigned numCutValues = 0;
  };
  mlir::FailureOr<Stats> run();

private:
  // Phase 0: Cut combinational cycles and sort the module
  llvm::DenseSet<Operation *> findCyclicOps();
  LogicalResult sortModule();

  // Phase 1: Collect i1 values and run simulation
  void collectValues();
  void buildSimNetwork();
  void runSimulation();
  void simulateWord(unsigned word);
  void addCounterexample();

  // Phase 2: Build equivalence classes from simulation
  void buildEquivalenceClasses();

  // Phase 3: SAT-based verification with incremental solvers on the cones
  void verifyCandidates();
  LogicalResult resetSATSolver();
  int getLiteral(unsigned node);
  void encodeNode(unsigned node);
  void collectProvenEquivalences();

  // Phase 4: Merge equivalent nodes
  void mergeEquivalentNodes();
  void replaceEquivalentNodes();

  // Test transformation helpers.
  static Attribute getTestEquivClass(Value value);
  static bool matchesTestEquivClass(Value lhs, Value rhs);
  EquivResult verifyEquivalence(unsigned lhs, unsigned rhs, bool inverted);

  // Module being processed
  hw::HWModuleOp module;

  // Configuration
  unsigned numPatterns;
  unsigned seed;
  bool testTransformation;
  bool useChoices;
  bool refine;
  unsigned maxSolverVars;
  bool orderFromOutputs;

  // Ops on combinational cycles of the network. Their results are cut
  // points: free inputs for simulation and SAT, never merged.
  llvm::DenseSet<Operation *> cutOps;

  // Primary inputs (block arguments, results of unknown operations and cut
  // points, treated as inputs)
  SmallVector<Value> primaryInputs;

  // All i1 values in topological order
  SmallVector<Value> allValues;
  llvm::DenseMap<Value, unsigned> valueIndex;

  // Flat simulation network over the indices of `allValues`. Fanins of
  // And/Or/Xor are encoded as (index << 1 | inverted); Generic nodes are
  // evaluated through BooleanLogicOpInterface with plain fanin indices.
  enum class SimKind : uint8_t { Input, Constant, And, Or, Xor, Generic };
  SmallVector<SimKind> simKind;
  SmallVector<unsigned> faninBegin;
  SmallVector<unsigned> fanins;
  SmallVector<uint8_t> constantValue;
  SmallVector<bool> eligible;

  // Simulation signatures, one 64-bit word per pattern block and node:
  // simWords[word][node]. The first numPatterns/64 words are random; the
  // following ones collect SAT counterexamples (one bit each).
  std::vector<std::vector<uint64_t>> simWords;
  unsigned numRandomWords = 0;
  unsigned numCexBits = 0;

  // Equivalence candidates: groups of value indices with identical or
  // inverted simulation signatures, tracked with an inversion flag relative
  // to the first member (the representative)
  SmallVector<SmallVector<std::pair<unsigned, bool>>> equivCandidates;

  // Proven merges: member index -> representative index (or -1) and whether
  // the member is the inverse of the representative.
  SmallVector<int> provenRep;
  SmallVector<bool> provenInv;
  // Candidate pairs (representative, member) not to check again.
  llvm::DenseSet<std::pair<unsigned, unsigned>> checkedPairs;

  // Proven equivalences: representative -> proven equivalent members with
  // inversion flag indicating whether the member is inverted relative to
  // representative
  llvm::MapVector<Value, SmallVector<std::pair<Value, bool>>>
      provenEquivalences;

  // The current SAT solver. It holds the CNF of the cones of the candidates
  // checked since it was created, with every proven member replaced by its
  // representative; it is replaced by a fresh one once it has more than
  // maxSolverVars variables (a satisfiable query assigns all of them).
  SATSolverFactory createSolver;
  std::unique_ptr<IncrementalSATSolver> satSolver;
  // Node index -> literal in the current solver (0 if not encoded).
  std::vector<int> nodeLiteral;
  int trueLiteral = 0;
  int numSolverVars = 0;
  std::mt19937_64 cexRng;
  // Run time profile (debug output only).
  struct {
    double encodeSeconds = 0, satSeconds = 0, unsatSeconds = 0;
    unsigned numTrivial = 0, numSolvers = 0;
  } profile;
  Stats stats;
};

Attribute FunctionalReductionSolver::getTestEquivClass(Value value) {
  Operation *op = value.getDefiningOp();
  if (!op)
    return {};
  return op->getAttr(kTestClassAttrName);
}

bool FunctionalReductionSolver::matchesTestEquivClass(Value lhs, Value rhs) {
  Attribute lhsClass = getTestEquivClass(lhs);
  Attribute rhsClass = getTestEquivClass(rhs);
  return lhsClass && rhsClass && lhsClass == rhsClass;
}

EquivResult FunctionalReductionSolver::verifyEquivalence(unsigned lhs,
                                                         unsigned rhs,
                                                         bool inverted) {

  if (testTransformation) {
    if (matchesTestEquivClass(allValues[lhs], allValues[rhs]))
      return EquivResult::Proved;
    return EquivResult::Unknown;
  }
  assert(satSolver && "SAT solver must be initialized before verification");
  if (maxSolverVars && numSolverVars > static_cast<int>(maxSolverVars))
    if (failed(resetSATSolver()))
      return EquivResult::Unknown;

  // SAT-based equivalence checking builds a miter for the two candidate nodes
  // and proves that no input assignment can make them differ.
  using Clock = std::chrono::steady_clock;
  auto start = Clock::now();
  int lhsLit = getLiteral(lhs);
  int rhsLit = getLiteral(rhs);
  if (inverted)
    rhsLit = -rhsLit;
  auto encoded = Clock::now();
  profile.encodeSeconds +=
      std::chrono::duration<double>(encoded - start).count();
  // Equal after replacing the proven members by their representatives.
  if (lhsLit == rhsLit) {
    ++profile.numTrivial;
    return EquivResult::Proved;
  }

  // miter -> (lhs != rhs)
  int miter = satSolver->newVar();
  ++numSolverVars;
  satSolver->addClause({-miter, lhsLit, rhsLit});
  satSolver->addClause({-miter, -lhsLit, -rhsLit});
  satSolver->assume(miter);
  auto result = satSolver->solve();
  double solveSeconds =
      std::chrono::duration<double>(Clock::now() - encoded).count();
  if (result == IncrementalSATSolver::kSAT) {
    profile.satSeconds += solveSeconds;
    return EquivResult::Disproved;
  }
  profile.unsatSeconds += solveSeconds;
  if (result != IncrementalSATSolver::kUNSAT)
    return EquivResult::Unknown;
  // Keep the proof as a lemma for later queries in this solver.
  satSolver->addClause({-miter});
  satSolver->addClause({-lhsLit, rhsLit});
  satSolver->addClause({lhsLit, -rhsLit});
  return EquivResult::Proved;
}

LogicalResult FunctionalReductionSolver::resetSATSolver() {
  satSolver = createSolver();
  if (!satSolver)
    return failure();
  ++profile.numSolvers;
  nodeLiteral.assign(allValues.size(), 0);
  numSolverVars = 0;
  trueLiteral = satSolver->newVar();
  ++numSolverVars;
  satSolver->addClause({trueLiteral});
  return success();
}

/// The literal of `node` in the current solver, encoding its cone first. A
/// proven member is encoded as its representative (possibly inverted).
int FunctionalReductionSolver::getLiteral(unsigned node) {
  bool inverted = false;
  while (provenRep[node] >= 0) {
    inverted ^= provenInv[node];
    node = provenRep[node];
  }
  if (!nodeLiteral[node])
    encodeNode(node);
  return inverted ? -nodeLiteral[node] : nodeLiteral[node];
}

void FunctionalReductionSolver::encodeNode(unsigned root) {
  constexpr int kOnStack = std::numeric_limits<int>::min();
  auto resolve = [&](unsigned node) {
    while (provenRep[node] >= 0)
      node = provenRep[node];
    return node;
  };
  auto newVar = [&]() {
    ++numSolverVars;
    return satSolver->newVar();
  };
  auto addClause = [&](ArrayRef<int> clause) { satSolver->addClause(clause); };

  // Iterative post-order over the (substituted) fanins. Nodes on the stack
  // are marked with kOnStack in `nodeLiteral`; a fanin found in that state
  // would close a cycle (cut points prevent this) and is left as a free
  // variable, which can only lose proofs.
  SmallVector<std::pair<unsigned, bool>> stack;
  stack.push_back({root, false});
  SmallVector<int> faninLits;
  while (!stack.empty()) {
    auto [node, expanded] = stack.back();
    if (!expanded) {
      if (nodeLiteral[node]) {
        stack.pop_back();
        continue;
      }
      SimKind kind = simKind[node];
      if (kind == SimKind::Input) {
        nodeLiteral[node] = newVar();
        stack.pop_back();
        continue;
      }
      if (kind == SimKind::Constant) {
        nodeLiteral[node] = constantValue[node] ? trueLiteral : -trueLiteral;
        stack.pop_back();
        continue;
      }
      stack.back().second = true;
      nodeLiteral[node] = kOnStack;
      for (unsigned i = faninBegin[node], e = faninBegin[node + 1]; i != e;
           ++i) {
        unsigned fanin = resolve(fanins[i] >> 1);
        if (!nodeLiteral[fanin])
          stack.push_back({fanin, false});
      }
      continue;
    }
    stack.pop_back();

    faninLits.clear();
    for (unsigned i = faninBegin[node], e = faninBegin[node + 1]; i != e; ++i) {
      unsigned fanin = fanins[i];
      bool inverted = fanin & 1;
      unsigned faninNode = fanin >> 1;
      while (provenRep[faninNode] >= 0) {
        inverted ^= provenInv[faninNode];
        faninNode = provenRep[faninNode];
      }
      int lit = nodeLiteral[faninNode];
      if (lit == kOnStack || lit == 0)
        lit = newVar();
      faninLits.push_back(inverted ? -lit : lit);
    }
    int out = newVar();
    nodeLiteral[node] = out;
    switch (simKind[node]) {
    case SimKind::And:
      circt::addAndClauses(out, faninLits, addClause);
      break;
    case SimKind::Or:
      circt::addOrClauses(out, faninLits, addClause);
      break;
    case SimKind::Xor:
      circt::addParityClauses(out, faninLits, addClause, newVar);
      break;
    case SimKind::Generic:
      cast<BooleanLogicOpInterface>(allValues[node].getDefiningOp())
          .emitCNF(out, faninLits, addClause, newVar);
      break;
    case SimKind::Input:
    case SimKind::Constant:
      llvm_unreachable("leaves are encoded when visited");
    }
  }
}

//===----------------------------------------------------------------------===//
// Phase 0: Cut combinational cycles and sort the module
//===----------------------------------------------------------------------===//

/// Strongly connected components of a graph given by fanin lists (CSR).
/// `sccNumber` numbers the components in a topological order (the
/// components of the fanins first) and `onCycle` marks the nodes in a
/// component with more than one node or with a self-loop. Iterative Tarjan.
static void computeSCCs(ArrayRef<unsigned> faninBegin,
                        ArrayRef<unsigned> faninList,
                        SmallVectorImpl<unsigned> &sccNumber,
                        SmallVectorImpl<bool> &onCycle) {
  unsigned numNodes = faninBegin.size() - 1;
  const unsigned unvisited = ~0U;
  SmallVector<unsigned> index(numNodes, unvisited), lowLink(numNodes);
  SmallVector<bool> onStack(numNodes, false), selfLoop(numNodes, false);
  sccNumber.assign(numNodes, 0);
  onCycle.assign(numNodes, false);
  SmallVector<unsigned> sccStack;
  SmallVector<std::pair<unsigned, unsigned>> callStack;
  unsigned counter = 0, numSCCs = 0;
  auto push = [&](unsigned v) {
    index[v] = lowLink[v] = counter++;
    sccStack.push_back(v);
    onStack[v] = true;
    callStack.push_back({v, faninBegin[v]});
  };
  for (unsigned root = 0; root != numNodes; ++root) {
    if (index[root] != unvisited)
      continue;
    push(root);
    while (!callStack.empty()) {
      unsigned v = callStack.back().first;
      unsigned pos = callStack.back().second;
      if (pos < faninBegin[v + 1]) {
        ++callStack.back().second;
        unsigned w = faninList[pos];
        if (w == v)
          selfLoop[v] = true;
        else if (index[w] == unvisited)
          push(w);
        else if (onStack[w])
          lowLink[v] = std::min(lowLink[v], index[w]);
        continue;
      }
      callStack.pop_back();
      if (!callStack.empty()) {
        unsigned parent = callStack.back().first;
        lowLink[parent] = std::min(lowLink[parent], lowLink[v]);
      }
      if (lowLink[v] != index[v])
        continue;
      unsigned first = sccStack.size();
      do
        --first;
      while (sccStack[first] != v);
      bool isCycle = sccStack.size() - first > 1 || selfLoop[v];
      for (unsigned i = first, e = sccStack.size(); i != e; ++i) {
        unsigned w = sccStack[i];
        onStack[w] = false;
        sccNumber[w] = numSCCs;
        onCycle[w] = isCycle;
      }
      ++numSCCs;
      sccStack.resize(first);
    }
  }
}

static bool isRegisterOp(Operation *op) {
  return isa<seq::CompRegOp, seq::CompRegClockEnabledOp, seq::FirRegOp>(op);
}

/// The ops of the body that lie on a combinational cycle of network ops
/// (in a strongly connected component with more than one op, or using their
/// own result).
llvm::DenseSet<Operation *> FunctionalReductionSolver::findCyclicOps() {
  Block *body = module.getBodyBlock();
  SmallVector<Operation *> ops;
  llvm::DenseMap<Operation *, unsigned> opIndex;
  for (Operation &op : *body)
    if (isFunctionalReductionNetworkOp(&op)) {
      opIndex[&op] = ops.size();
      ops.push_back(&op);
    }
  SmallVector<unsigned> faninBegin, faninList;
  faninBegin.reserve(ops.size() + 1);
  for (Operation *op : ops) {
    faninBegin.push_back(faninList.size());
    for (Value operand : op->getOperands()) {
      Operation *def = operand.getDefiningOp();
      if (!def || def->getBlock() != body)
        continue;
      auto it = opIndex.find(def);
      if (it != opIndex.end())
        faninList.push_back(it->second);
    }
  }
  faninBegin.push_back(faninList.size());

  SmallVector<unsigned> sccNumber;
  SmallVector<bool> onCycle;
  computeSCCs(faninBegin, faninList, sccNumber, onCycle);
  llvm::DenseSet<Operation *> cyclic;
  for (auto [index, op] : llvm::enumerate(ops))
    if (onCycle[index])
      cyclic.insert(op);
  return cyclic;
}

/// A topological order of the strongly connected components of all ops of
/// the body over their combinational def-use edges (edges into registers
/// excluded; uses in nested regions count as uses by the op holding them):
/// an op can only reach ops with the same or a higher number.
static llvm::DenseMap<Operation *, unsigned>
computeCombinationalOrder(Block *body) {
  llvm::DenseMap<Operation *, unsigned> opIndex;
  SmallVector<Operation *> ops;
  for (Operation &op : *body) {
    opIndex[&op] = ops.size();
    ops.push_back(&op);
  }
  SmallVector<unsigned> faninBegin, faninList;
  faninBegin.reserve(ops.size() + 1);
  auto addFanin = [&](Value operand) {
    Operation *def = operand.getDefiningOp();
    if (!def || def->getBlock() != body)
      return;
    faninList.push_back(opIndex.lookup(def));
  };
  for (Operation *op : ops) {
    faninBegin.push_back(faninList.size());
    if (isRegisterOp(op))
      continue;
    if (op->getNumRegions() == 0) {
      for (Value operand : op->getOperands())
        addFanin(operand);
      continue;
    }
    op->walk([&](Operation *nested) {
      for (Value operand : nested->getOperands())
        addFanin(operand);
    });
  }
  faninBegin.push_back(faninList.size());

  SmallVector<unsigned> sccNumber;
  SmallVector<bool> onCycle;
  computeSCCs(faninBegin, faninList, sccNumber, onCycle);
  llvm::DenseMap<Operation *, unsigned> order;
  order.reserve(ops.size());
  for (auto [index, op] : llvm::enumerate(ops))
    order[op] = sccNumber[index];
  return order;
}

/// Sort the network ops topologically; results of cut ops count as defined.
LogicalResult FunctionalReductionSolver::sortModule() {
  auto isOperandReady = [&](Value value, Operation *op) -> bool {
    if (!isFunctionalReductionNetworkOp(op))
      return true;
    Operation *def = value.getDefiningOp();
    return def && cutOps.contains(def);
  };
  if (failed(topologicallySortGraphRegionBlocks(module, isOperandReady)))
    return module->emitError()
           << "FunctionalReduction: Failed to topologically sort logic network";
  return success();
}

//===----------------------------------------------------------------------===//
// Phase 1: Collect values and run simulation
//===----------------------------------------------------------------------===//

/// The gates in a depth-first post-order from the outputs: the operands of
/// the terminator, of the other ops without results (assumptions,
/// assertions) and the inputs of the registers (bit by bit, least
/// significant first), visiting the operands of an op in block order. Gates
/// not reached follow in their given order. (This is the order in which ABC
/// numbers the nodes of an AIG.)
static SmallVector<Operation *>
orderGatesFromOutputs(Block *body, ArrayRef<Operation *> gates) {
  llvm::DenseMap<Operation *, unsigned> position;
  for (auto [index, op] : llvm::enumerate(*body))
    position[&op] = index;
  llvm::DenseSet<Operation *> isGate(gates.begin(), gates.end());
  llvm::DenseSet<Operation *> visited;
  SmallVector<Operation *> order;
  SmallVector<std::pair<Operation *, SmallVector<Operation *, 4>>> stack;
  auto fanins = [&](Operation *op) {
    SmallVector<Operation *, 4> result;
    for (Value operand : op->getOperands())
      if (Operation *def = operand.getDefiningOp();
          def && def->getBlock() == body && !isRegisterOp(def))
        result.push_back(def);
    // Visited from the back: the earliest operand first.
    llvm::sort(result, [&](Operation *lhs, Operation *rhs) {
      return position.lookup(lhs) > position.lookup(rhs);
    });
    return result;
  };
  auto visit = [&](Value root) {
    Operation *rootOp = root.getDefiningOp();
    if (!rootOp || rootOp->getBlock() != body || isRegisterOp(rootOp) ||
        !visited.insert(rootOp).second)
      return;
    stack.push_back({rootOp, fanins(rootOp)});
    while (!stack.empty()) {
      auto &pending = stack.back().second;
      if (!pending.empty()) {
        Operation *def = pending.pop_back_val();
        if (visited.insert(def).second)
          stack.push_back({def, fanins(def)});
        continue;
      }
      if (isGate.contains(stack.back().first))
        order.push_back(stack.back().first);
      stack.pop_back();
    }
  };
  auto visitBits = [&](Value value) {
    if (auto concat = value.getDefiningOp<comb::ConcatOp>()) {
      for (Value operand : llvm::reverse(concat.getOperands()))
        visit(operand);
      return;
    }
    visit(value);
  };
  for (Value operand : body->getTerminator()->getOperands())
    visitBits(operand);
  for (Operation &op : *body)
    if (op.getNumResults() == 0 && &op != body->getTerminator())
      for (Value operand : op.getOperands())
        visitBits(operand);
  for (Operation &op : *body)
    if (isRegisterOp(&op) && op.getNumOperands())
      visitBits(op.getOperand(0));
  for (Operation *op : gates)
    if (!visited.contains(op))
      order.push_back(op);
  return order;
}

void FunctionalReductionSolver::collectValues() {

  // Seed zero constants so nodes can be merged
  // if input IR does not contain constants already.
  OpBuilder builder(module.getContext());
  builder.setInsertionPointToStart(module.getBodyBlock());
  auto i1Type = builder.getIntegerType(1);
  hw::ConstantOp::create(builder, module.getLoc(), i1Type, 0);

  // Collect block arguments (primary inputs) that are i1
  for (auto arg : module.getBodyBlock()->getArguments()) {
    if (arg.getType().isInteger(1)) {
      primaryInputs.push_back(arg);
      allValues.push_back(arg);
    }
  }

  // Walk operations and collect i1 results
  // - Constants, unknown operations and cut points: treat as inputs (assign
  //   random patterns). They come first, so that a gate equivalent to an
  //   input or constant is replaced by it rather than the other way round.
  // - AIG operations: add to allValues for simulation, in topological order
  SmallVector<Operation *> gates;
  module.walk([&](Operation *op) {
    bool isGate =
        isFunctionalReductionSimulatableOp(op) && !cutOps.contains(op);
    if (isGate) {
      gates.push_back(op);
      return;
    }
    for (auto result : op->getResults()) {
      if (!result.getType().isInteger(1))
        continue;

      allValues.push_back(result);
      if (cutOps.contains(op)) {
        primaryInputs.push_back(result);
        ++stats.numCutValues;
      } else if (!op->hasTrait<OpTrait::ConstantLike>()) {
        // Unknown operations - treat as primary inputs
        primaryInputs.push_back(result);
      }
    }
  });
  if (orderFromOutputs)
    gates = orderGatesFromOutputs(module.getBodyBlock(), gates);
  for (Operation *op : gates)
    for (auto result : op->getResults())
      if (result.getType().isInteger(1))
        allValues.push_back(result);

  valueIndex.reserve(allValues.size());
  for (auto [index, value] : llvm::enumerate(allValues))
    valueIndex[value] = index;

  LLVM_DEBUG(llvm::dbgs() << "FunctionalReduction: Collected "
                          << primaryInputs.size()
                          << " primary inputs (including unknown ops) and "
                          << allValues.size() << " total i1 values\n");
}

void FunctionalReductionSolver::buildSimNetwork() {
  unsigned numValues = allValues.size();
  simKind.assign(numValues, SimKind::Input);
  constantValue.assign(numValues, 0);
  eligible.assign(numValues, true);
  faninBegin.clear();
  faninBegin.reserve(numValues + 1);
  fanins.clear();

  // Fanin (index << 1 | inverted), or false if `input` was not collected.
  auto addFanin = [&](Value input, bool inverted) {
    auto it = valueIndex.find(input);
    if (it == valueIndex.end())
      return false;
    fanins.push_back(it->second << 1 | (inverted ? 1 : 0));
    return true;
  };

  llvm::DenseMap<std::pair<Value, unsigned>, unsigned> firstExtract;
  for (auto [index, value] : llvm::enumerate(allValues)) {
    faninBegin.push_back(fanins.size());
    Operation *op = value.getDefiningOp();
    if (!op)
      continue;
    if (cutOps.contains(op)) {
      eligible[index] = false;
      continue;
    }
    APInt constant;
    if (matchPattern(value, mlir::m_ConstantInt(&constant))) {
      simKind[index] = SimKind::Constant;
      constantValue[index] = !constant.isZero();
      continue;
    }
    if (!isFunctionalReductionSimulatableOp(op)) {
      // Duplicate extracts of the same bit (not CSE'd) are the same input:
      // simulate and encode the later ones as buffers of the first.
      if (auto extract = dyn_cast<comb::ExtractOp>(op)) {
        auto [it, inserted] = firstExtract.try_emplace(
            {extract.getInput(), extract.getLowBit()}, index);
        if (!inserted) {
          fanins.push_back(it->second << 1);
          simKind[index] = SimKind::And;
        }
      }
      continue;
    }

    SimKind kind =
        TypeSwitch<Operation *, SimKind>(op)
            .Case<aig::AndInverterOp>([&](auto andOp) {
              for (auto [i, input] : llvm::enumerate(andOp.getInputs()))
                if (!addFanin(input, andOp.isInverted(i)))
                  return SimKind::Input;
              return SimKind::And;
            })
            .Case<comb::AndOp, comb::OrOp, comb::XorOp>([&](auto op) {
              for (auto input : op.getInputs())
                if (!addFanin(input, false))
                  return SimKind::Input;
              return isa<comb::AndOp>(op)  ? SimKind::And
                     : isa<comb::OrOp>(op) ? SimKind::Or
                                           : SimKind::Xor;
            })
            .Case<BooleanLogicOpInterface>([&](auto logicOp) {
              for (auto input : logicOp.getInputs())
                if (!addFanin(input, false))
                  return SimKind::Input;
              return SimKind::Generic;
            })
            .Default([](Operation *) { return SimKind::Input; });
    if (kind == SimKind::Input)
      fanins.resize(faninBegin.back());
    simKind[index] = kind;
  }
  faninBegin.push_back(fanins.size());
}

void FunctionalReductionSolver::simulateWord(unsigned word) {
  uint64_t *sim = simWords[word].data();
  SmallVector<APInt, 4> genericInputs;
  for (unsigned node = 0, e = allValues.size(); node != e; ++node) {
    ArrayRef<unsigned> nodeFanins(fanins.data() + faninBegin[node],
                                  fanins.data() + faninBegin[node + 1]);
    auto faninWord = [&](unsigned fanin) -> uint64_t {
      uint64_t value = sim[fanin >> 1];
      return (fanin & 1) ? ~value : value;
    };
    switch (simKind[node]) {
    case SimKind::Input:
      break;
    case SimKind::Constant:
      sim[node] = constantValue[node] ? ~uint64_t(0) : 0;
      break;
    case SimKind::And: {
      uint64_t result = ~uint64_t(0);
      for (unsigned fanin : nodeFanins)
        result &= faninWord(fanin);
      sim[node] = result;
      break;
    }
    case SimKind::Or: {
      uint64_t result = 0;
      for (unsigned fanin : nodeFanins)
        result |= faninWord(fanin);
      sim[node] = result;
      break;
    }
    case SimKind::Xor: {
      uint64_t result = 0;
      for (unsigned fanin : nodeFanins)
        result ^= faninWord(fanin);
      sim[node] = result;
      break;
    }
    case SimKind::Generic: {
      genericInputs.clear();
      for (unsigned fanin : nodeFanins)
        genericInputs.push_back(APInt(64, faninWord(fanin)));
      auto logicOp =
          cast<BooleanLogicOpInterface>(allValues[node].getDefiningOp());
      sim[node] = logicOp
                      .evaluateBooleanLogic([&](unsigned i) -> const APInt & {
                        return genericInputs[i];
                      })
                      .getZExtValue();
      break;
    }
    }
  }
}

void FunctionalReductionSolver::runSimulation() {
  // Calculate number of 64-bit words needed for numPatterns bits
  numRandomWords = numPatterns / 64;
  simWords.assign(numRandomWords, std::vector<uint64_t>(allValues.size(), 0));

  // Create seeded random number generator for deterministic patterns
  std::mt19937_64 rng(seed);
  llvm::DenseSet<unsigned> assigned;
  auto assignRandom = [&](unsigned node) {
    for (auto &words : simWords)
      words[node] = rng();
  };
  for (auto input : primaryInputs) {
    unsigned node = valueIndex.at(input);
    assigned.insert(node);
    assignRandom(node);
  }
  // Supported ops whose operands could not be collected are simulated as
  // inputs as well.
  for (unsigned node = 0, e = allValues.size(); node != e; ++node)
    if (simKind[node] == SimKind::Input && !assigned.contains(node))
      assignRandom(node);

  // Propagate simulation through the circuit in topological order
  for (unsigned word = 0; word != numRandomWords; ++word)
    simulateWord(word);

  LLVM_DEBUG({
    llvm::dbgs() << "FunctionalReduction: Simulation complete with "
                 << numPatterns << " patterns\n";
  });
}

/// Add the input assignment of the last satisfiable SAT call as a new
/// simulation pattern and simulate it.
void FunctionalReductionSolver::addCounterexample() {
  unsigned bit = numCexBits++ % 64;
  if (bit == 0)
    simWords.emplace_back(allValues.size(), 0);
  auto &words = simWords.back();
  // Inputs outside the cones of the query get random values.
  uint64_t random = 0;
  for (unsigned node = 0, e = allValues.size(); node != e; ++node) {
    if (simKind[node] != SimKind::Input)
      continue;
    bool value;
    if (int lit = nodeLiteral[node]; lit > 0) {
      value = satSolver->val(lit) > 0;
    } else {
      if (node % 64 == 0 || !random)
        random = cexRng();
      value = random & 1;
      random >>= 1;
    }
    if (value)
      words[node] |= uint64_t(1) << bit;
  }
  simulateWord(simWords.size() - 1);
}

//===----------------------------------------------------------------------===//
// Phase 2: Build equivalence classes from simulation
//===----------------------------------------------------------------------===//

void FunctionalReductionSolver::buildEquivalenceClasses() {
  // Group the nodes that are not merged yet by their canonical signature.
  // Inverted signals share the same canonical signature since inversion
  // is zero cost in synthesis; the phase of a signature is the most
  // significant bit of the random patterns.
  equivCandidates.clear();
  unsigned numWords = simWords.size();
  auto phaseOf = [&](unsigned node) -> bool {
    return simWords[numRandomWords - 1][node] >> 63;
  };
  auto sameSignature = [&](unsigned lhs, unsigned rhs, bool inverted) {
    uint64_t mask = inverted ? ~uint64_t(0) : 0;
    for (unsigned word = 0; word != numWords; ++word)
      if (simWords[word][lhs] != (simWords[word][rhs] ^ mask))
        return false;
    return true;
  };

  llvm::DenseMap<uint64_t, SmallVector<unsigned, 1>> buckets;
  SmallVector<SmallVector<std::pair<unsigned, bool>>> groups;
  SmallVector<bool> groupPhase;
  for (unsigned node = 0, e = allValues.size(); node != e; ++node) {
    if (!eligible[node] || provenRep[node] >= 0)
      continue;
    bool phase = phaseOf(node);
    uint64_t mask = phase ? ~uint64_t(0) : 0;
    uint64_t hash = 0;
    for (unsigned word = 0; word != numWords; ++word)
      hash = llvm::hash_combine(hash, simWords[word][node] ^ mask);
    auto &bucket = buckets[hash];
    bool found = false;
    for (unsigned group : bucket) {
      unsigned rep = groups[group].front().first;
      bool inverted = phase != groupPhase[group];
      if (sameSignature(rep, node, inverted)) {
        groups[group].push_back({node, inverted});
        found = true;
        break;
      }
    }
    if (found)
      continue;
    bucket.push_back(groups.size());
    groups.push_back({{node, false}});
    groupPhase.push_back(phase);
  }

  // Build equivalence candidates for groups with >1 member, in the order of
  // their representatives.
  for (auto &members : groups)
    if (members.size() > 1)
      equivCandidates.push_back(std::move(members));

  LLVM_DEBUG(llvm::dbgs() << "FunctionalReduction: Built "
                          << equivCandidates.size()
                          << " equivalence candidates\n");
}

//===----------------------------------------------------------------------===//
// Phase 3: SAT-based verification
//
// Every member of a candidate class is checked against the representative,
// in topological order of the members, so that the lemmas of the proven
// equivalences in a cone are available for the queries above it. With
// `refine`, each counterexample is simulated at once (later candidates it
// separates need no SAT call) and the classes are rebuilt and checked again
// until a round finds no counterexample.
//===----------------------------------------------------------------------===//

void FunctionalReductionSolver::verifyCandidates() {
  LLVM_DEBUG(
      llvm::dbgs() << "FunctionalReduction: Starting SAT verification with "
                   << equivCandidates.size() << " equivalence classes\n");

  struct Candidate {
    unsigned member;
    unsigned representative;
    bool inverted;
  };
  SmallVector<Candidate> candidates;
  double satSeconds = 0, cexSeconds = 0;
  while (true) {
    ++stats.numRounds;
    if (stats.numRounds > 1)
      buildEquivalenceClasses();

    candidates.clear();
    for (auto &members : equivCandidates) {
      auto [representative, repInversion] = members.front();
      assert(!repInversion && "representative must not be inverted");
      (void)repInversion;
      for (auto [member, inverted] :
           llvm::ArrayRef<std::pair<unsigned, bool>>(members).drop_front())
        if (!checkedPairs.contains({representative, member}))
          candidates.push_back({member, representative, inverted});
    }
    if (candidates.empty())
      break;
    llvm::sort(candidates, [](const Candidate &lhs, const Candidate &rhs) {
      return lhs.member < rhs.member;
    });

    // Words of counterexamples found in this round, which the classes do
    // not reflect yet.
    unsigned firstNewWord = simWords.size();
    if (numCexBits % 64 != 0)
      --firstNewWord;
    unsigned roundCex = 0;
    for (auto [member, representative, inverted] : candidates) {
      uint64_t mask = inverted ? ~uint64_t(0) : 0;
      bool separated = false;
      for (unsigned word = firstNewWord, e = simWords.size(); word != e; ++word)
        if (simWords[word][representative] != (simWords[word][member] ^ mask)) {
          separated = true;
          break;
        }
      if (separated) {
        ++stats.numSimDisproved;
        continue;
      }

      checkedPairs.insert({representative, member});
      auto satStart = std::chrono::steady_clock::now();
      EquivResult result = verifyEquivalence(representative, member, inverted);
      satSeconds += std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - satStart)
                        .count();
      if (result == EquivResult::Proved) {
        stats.numProvedEquiv++;
        provenRep[member] = representative;
        provenInv[member] = inverted;
      } else if (result == EquivResult::Disproved) {
        stats.numDisprovedEquiv++;
        if (refine) {
          auto cexStart = std::chrono::steady_clock::now();
          addCounterexample();
          LLVM_DEBUG({
            uint64_t mask = inverted ? ~uint64_t(0) : 0;
            if (simWords.back()[representative] ==
                (simWords.back()[member] ^ mask))
              llvm::dbgs() << "FunctionalReduction: counterexample does not "
                              "separate "
                           << allValues[representative] << " and "
                           << allValues[member] << "\n";
          });
          cexSeconds += std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - cexStart)
                            .count();
          ++roundCex;
        }
      } else {
        stats.numUnknown++;
      }
    }
    if (!refine || roundCex == 0)
      break;
  }
  (void)satSeconds;
  (void)cexSeconds;

  LLVM_DEBUG(
      llvm::dbgs() << "FunctionalReduction: SAT verification complete. Proved "
                   << stats.numProvedEquiv << " equivalences in "
                   << stats.numRounds << " rounds (SAT " << satSeconds
                   << " s, counterexample simulation " << cexSeconds << " s)\n"
                   << "  encoding " << profile.encodeSeconds << " s, SAT "
                   << profile.satSeconds << " s, UNSAT/unknown "
                   << profile.unsatSeconds << " s, structurally equal "
                   << profile.numTrivial << ", solvers " << profile.numSolvers
                   << "\n");
}

/// Resolve chains of merges (a representative merged later into an earlier
/// one) and group the members by their final representative, in
/// topological order.
void FunctionalReductionSolver::collectProvenEquivalences() {
  struct Merge {
    unsigned representative;
    unsigned member;
    bool inverted;
  };
  SmallVector<Merge> merges;
  for (unsigned node = 0, e = allValues.size(); node != e; ++node) {
    if (provenRep[node] < 0)
      continue;
    unsigned representative = provenRep[node];
    bool inverted = provenInv[node];
    // Representatives precede their members; chains end at a node that is
    // not merged.
    while (provenRep[representative] >= 0) {
      inverted ^= provenInv[representative];
      representative = provenRep[representative];
    }
    provenRep[node] = representative;
    provenInv[node] = inverted;
    merges.push_back({representative, node, inverted});
  }
  llvm::stable_sort(merges, [](const Merge &lhs, const Merge &rhs) {
    return lhs.representative < rhs.representative;
  });
  for (auto &merge : merges)
    provenEquivalences[allValues[merge.representative]].push_back(
        {allValues[merge.member], merge.inverted});
}

//===----------------------------------------------------------------------===//
// Phase 4: Merge equivalent nodes
//===----------------------------------------------------------------------===//

void FunctionalReductionSolver::mergeEquivalentNodes() {
  if (provenEquivalences.empty())
    return;

  // Build all replacement IR first, then perform use rewrites in a second
  // phase. This keeps `isBeforeInBlock` queries anchored to the final block
  // order instead of an order that is still being mutated by insertion.
  struct PlannedMember {
    Value original;
    bool inverted;
    aig::AndInverterOp operandInverter;
  };
  struct MergeRewritePlan {
    Value representative;
    SmallVector<PlannedMember> members;
    // Members which are at risk of reaching their representative
    SmallVector<PlannedMember> reachableMembers;
    synth::ChoiceOp choice;
    aig::AndInverterOp choiceNot;
  };

  mlir::OpBuilder builder(module.getContext());
  auto replaceDominatedUses =
      [](Value from, Value to,
         llvm::function_ref<bool(Operation *)> shouldReplaceOwner) {
        auto *defOp = to.getDefiningOp();
        assert(defOp && "replacement value must be defined by an operation");
        from.replaceUsesWithIf(to, [&](OpOperand &use) {
          auto *user = use.getOwner();
          // Restrict rewrites to uses after the replacement value's definition
          // in the same block so merging cannot introduce use-before-def edges
          // or SSA cycles.
          return shouldReplaceOwner(user) &&
                 user->getBlock() == defOp->getBlock();
        });
      };

  DenseSet<Value> reachable;
  auto visitFrom = [&](Value start) {
    SmallVector<Value> stack;
    stack.push_back(start);
    while (!stack.empty()) {
      Value current = stack.pop_back_val();
      if (!reachable.insert(current).second)
        continue;
      for (Operation *user : current.getUsers())
        if (isLogicNetworkOp(user))
          for (Value result : user->getResults())
            stack.push_back(result);
    }
  };

  SmallVector<MergeRewritePlan> rewritePlans;
  rewritePlans.reserve(provenEquivalences.size());
  for (auto provenEquivSet : provenEquivalences) {
    auto &[representative, members] = provenEquivSet;
    if (members.empty())
      continue;
    // Mark all values reachable from representative before checking members.
    visitFrom(representative);

    // Greedily filter for members that can create a cycle with representative
    SmallVector<std::pair<Value, bool>> safeMembers;
    SmallVector<PlannedMember> plannedReachable;
    for (auto [member, inverted] : members) {
      if (reachable.count(member)) {
        plannedReachable.push_back({member, inverted, {}});
        continue;
      }
      visitFrom(member); // Visit users
      safeMembers.push_back({member, inverted});
    }

    if (safeMembers.empty())
      continue;

    builder.setInsertionPointAfterValue(safeMembers.back().first);

    SmallVector<Value> operands;
    operands.reserve(safeMembers.size() + 1);
    operands.push_back(representative);

    SmallVector<PlannedMember> plannedMembers;
    plannedMembers.reserve(safeMembers.size());
    bool hasInvertedMember = false;
    for (auto [member, inverted] : safeMembers) {
      auto &planned =
          plannedMembers.emplace_back(PlannedMember{member, inverted, {}});
      if (!inverted) {
        operands.push_back(member);
        continue;
      }
      hasInvertedMember = true;
      // If the member is inverted relative to the representative, we
      // create an inverter for the choice operand
      planned.operandInverter =
          aig::AndInverterOp::create(builder, member.getLoc(), member, true);
      operands.push_back(planned.operandInverter.getResult());
    }

    auto choice = synth::ChoiceOp::create(builder, representative.getLoc(),
                                          representative.getType(), operands);

    // If there is an inverted member, we need to create an inverter for the
    // choice result as well
    auto choiceNot = !hasInvertedMember
                         ? nullptr
                         : aig::AndInverterOp::create(builder, choice.getLoc(),
                                                      choice, true);

    stats.numMergedNodes += safeMembers.size() + 1;
    rewritePlans.push_back({representative, std::move(plannedMembers),
                            std::move(plannedReachable), choice, choiceNot});
  }

  for (auto &plan : rewritePlans) {
    auto replaceValue = [&](const PlannedMember &member) {
      if (member.inverted)
        replaceDominatedUses(member.original, plan.choiceNot,
                             [&](Operation *user) {
                               // Do not rewrite the freshly created operand
                               // inverter or the choice result inverter. This
                               // avoids creating an immediate cycle when
                               // merging an inverted node into its
                               // representative.
                               return user != member.operandInverter &&
                                      user != plan.choiceNot.getOperation();
                             });
      else
        replaceDominatedUses(member.original, plan.choice,
                             [&](Operation *user) {
                               return user != plan.choice.getOperation();
                             });
    };

    replaceDominatedUses(
        plan.representative, plan.choice,
        [&](Operation *user) { return user != plan.choice.getOperation(); });
    for (const auto &member : plan.members)
      replaceValue(member);

    // Reachable members are redundant here so either replace their uses with
    // choice or erase if they have no uses left.
    for (auto &member : plan.reachableMembers) {
      member.original.replaceUsesWithIf(plan.choice, [&](OpOperand &use) {
        auto *user = use.getOwner();
        return user->getBlock() == plan.choice->getBlock();
      });
      if (member.original.use_empty())
        member.original.getDefiningOp()->erase();
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "FunctionalReduction: Merged "
                          << stats.numMergedNodes << " nodes\n");
}

/// Replace each proven member by its representative (or the inverse of it),
/// without choices. A replacement is skipped if the representative depends
/// combinationally on the member in the current IR, which would create a
/// cycle; registers break such paths.
void FunctionalReductionSolver::replaceEquivalentNodes() {
  if (provenEquivalences.empty())
    return;

  // Whether `target` is reachable from the users of `from` through
  // combinational ops. Ops earlier in the combinational order cannot be
  // reached; the order stays valid while every replacement only adds edges
  // to ops later in the order.
  Block *body = module.getBodyBlock();
  auto order = computeCombinationalOrder(body);
  bool orderValid = true;
  auto earlier = [&](Operation *lhs, Value rhs) {
    auto *rhsOp = rhs.getDefiningOp();
    if (!orderValid || !rhsOp)
      return false;
    auto lhsIt = order.find(lhs), rhsIt = order.find(rhsOp);
    return lhsIt != order.end() && rhsIt != order.end() &&
           lhsIt->second < rhsIt->second;
  };
  DenseSet<Operation *> visited;
  auto reaches = [&](Value from, Operation *target) {
    if (earlier(target, from))
      return false;
    visited.clear();
    SmallVector<Operation *> stack(from.getUsers().begin(),
                                   from.getUsers().end());
    while (!stack.empty()) {
      auto *op = stack.pop_back_val();
      if (op == target)
        return true;
      if (isRegisterOp(op) || !visited.insert(op).second)
        continue;
      for (auto result : op->getResults())
        stack.append(result.getUsers().begin(), result.getUsers().end());
    }
    return false;
  };

  // Only values and uses in the body itself (not in nested regions such as
  // seq.initial) are rewritten.
  auto inBody = [&](Value value) { return value.getParentBlock() == body; };
  OpBuilder builder(module.getContext());
  for (auto &[representative, members] : provenEquivalences) {
    if (!inBody(representative))
      continue;
    Value inverse;
    for (auto [member, inverted] : members) {
      auto *repOp = representative.getDefiningOp();
      if (!inBody(member) || (repOp && reaches(member, repOp)))
        continue;
      // The replacement adds edges from the representative to the users of
      // the member; the order stays valid if they all come after it.
      if (repOp && orderValid) {
        auto repIt = order.find(repOp);
        orderValid = repIt != order.end() &&
                     llvm::all_of(member.getUsers(), [&](Operation *user) {
                       auto it = order.find(user);
                       return it != order.end() && it->second > repIt->second;
                     });
      }
      Value replacement = representative;
      if (inverted) {
        if (!inverse) {
          if (repOp)
            builder.setInsertionPointAfter(repOp);
          else
            builder.setInsertionPointToStart(module.getBodyBlock());
          inverse = aig::AndInverterOp::create(builder, member.getLoc(),
                                               representative, true);
        }
        replacement = inverse;
      }
      member.replaceUsesWithIf(replacement, [&](OpOperand &use) {
        return use.getOwner()->getBlock() == body;
      });
      ++stats.numMergedNodes;
      if (auto *op = member.getDefiningOp();
          op && op->use_empty() && isLogicNetworkOp(op))
        op->erase();
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "FunctionalReduction: Replaced "
                          << stats.numMergedNodes << " nodes\n");
}

//===----------------------------------------------------------------------===//
// Main Functional Reduction algorithm
//===----------------------------------------------------------------------===//

mlir::FailureOr<FunctionalReductionSolver::Stats>
FunctionalReductionSolver::run() {
  LLVM_DEBUG(
      llvm::dbgs() << "FunctionalReduction: Starting functional reduction with "
                   << numPatterns << " simulation patterns\n");

  if (!testTransformation && !createSolver) {
    module->emitError()
        << "FunctionalReduction requires a SAT solver, but none is "
           "available in this build";
    return failure();
  }

  using Clock = std::chrono::steady_clock;
  auto phaseStart = Clock::now();
  auto endPhase = [&](StringRef name) {
    (void)name;
    LLVM_DEBUG({
      auto now = Clock::now();
      llvm::dbgs() << "FunctionalReduction: " << name << " took "
                   << std::chrono::duration<double>(now - phaseStart).count()
                   << " s\n";
    });
    phaseStart = Clock::now();
  };

  // Cut combinational cycles, then topologically sort the values
  cutOps = findCyclicOps();
  if (!cutOps.empty())
    LLVM_DEBUG(llvm::dbgs() << "FunctionalReduction: " << cutOps.size()
                            << " ops on combinational cycles are cut\n");
  if (failed(sortModule()))
    return failure();

  // Phase 1: Collect values and run simulation
  collectValues();
  if (allValues.empty()) {
    LLVM_DEBUG(llvm::dbgs()
               << "FunctionalReduction: No i1 values to process\n");
    return stats;
  }

  buildSimNetwork();
  runSimulation();
  endPhase("collection and simulation");

  // Phase 2: Build equivalence classes
  provenRep.assign(allValues.size(), -1);
  provenInv.assign(allValues.size(), false);
  buildEquivalenceClasses();
  stats.numEquivClasses = equivCandidates.size();
  if (equivCandidates.empty()) {
    LLVM_DEBUG(llvm::dbgs()
               << "FunctionalReduction: No equivalence candidates found\n");
    return stats;
  }

  // Phase 3: SAT-based verification
  if (!testTransformation && failed(resetSATSolver())) {
    module->emitError()
        << "FunctionalReduction requires a SAT solver, but none is "
           "available in this build";
    return failure();
  }
  verifyCandidates();
  collectProvenEquivalences();
  endPhase("verification");

  // Phase 4: Merge equivalent nodes
  if (useChoices)
    mergeEquivalentNodes();
  else
    replaceEquivalentNodes();
  endPhase("merging");

  // Re-sort after merging to restore topological order after choice insertion.
  if (failed(sortModule()))
    return failure();
  // Merges never close a new cycle; with cycles, check that no op joined
  // one (the sort above does not see cycles through cut points).
  if (!cutOps.empty())
    for (Operation *op : findCyclicOps())
      if (!cutOps.contains(op))
        return op->emitError()
               << "FunctionalReduction: merging created a combinational cycle";
  endPhase("sorting");

  LLVM_DEBUG(llvm::dbgs() << "FunctionalReduction: Complete. Stats:\n"
                          << "  Equivalence classes: " << stats.numEquivClasses
                          << "\n"
                          << "  Proved: " << stats.numProvedEquiv << "\n"
                          << "  Disproved: " << stats.numDisprovedEquiv << "\n"
                          << "  Unknown (limit): " << stats.numUnknown << "\n"
                          << "  Merged: " << stats.numMergedNodes << "\n");

  return stats;
}

//===----------------------------------------------------------------------===//
// Pass implementation
//===----------------------------------------------------------------------===//

struct FunctionalReductionPass
    : public circt::synth::impl::FunctionalReductionBase<
          FunctionalReductionPass> {
  using FunctionalReductionBase::FunctionalReductionBase;
  void updateStats(const FunctionalReductionSolver::Stats &stats) {
    numEquivClasses += stats.numEquivClasses;
    numProvedEquiv += stats.numProvedEquiv;
    numDisprovedEquiv += stats.numDisprovedEquiv;
    numUnknown += stats.numUnknown;
    numMergedNodes += stats.numMergedNodes;
    numSimDisproved += stats.numSimDisproved;
    numRounds += stats.numRounds;
    numCutValues += stats.numCutValues;
  }

  void runOnOperation() override {
    auto module = getOperation();
    LLVM_DEBUG(llvm::dbgs() << "Running FunctionalReduction pass on "
                            << module.getName() << "\n");

    if (numRandomPatterns == 0 || (numRandomPatterns & 63U) != 0) {
      module.emitError()
          << "'num-random-patterns' must be a positive multiple of 64";
      return signalPassFailure();
    }
    if (conflictLimit < -1) {
      module.emitError()
          << "'conflict-limit' must be greater than or equal to -1";
      return signalPassFailure();
    }

    SATSolverFactory createSolver;
    if (!testTransformation) {
      CadicalSATSolverOptions cadicalOptions;
      // Many small queries with a conflict limit: CaDiCaL's preprocessing
      // and inprocessing (run on the first query of every solver) and its
      // lucky phases only cost time here.
      cadicalOptions.luckyPhases = luckyPhases;
      cadicalOptions.config =
          CadicalSATSolverOptions::CadicalSolverConfig::Plain;
      createSolver = [backend = std::string(this->satSolver), cadicalOptions,
                      limit = static_cast<int>(conflictLimit)]() {
        auto solver = createSATSolver(backend, cadicalOptions);
        if (solver)
          solver->setConflictLimit(limit);
        return solver;
      };
      if (!createSolver()) {
        module.emitError() << "unsupported or unavailable SAT solver '"
                           << this->satSolver
                           << "' (expected auto, z3, or cadical)";
        return signalPassFailure();
      }
    }

    FunctionalReductionSolver fcSolver(module, numRandomPatterns, seed,
                                       testTransformation,
                                       std::move(createSolver), useChoices,
                                       refine, maxSolverVars, orderFromOutputs);
    auto stats = fcSolver.run();
    if (failed(stats))
      return signalPassFailure();
    updateStats(*stats);
    if (stats->numMergedNodes == 0)
      markAllAnalysesPreserved();
  }
};

} // namespace
