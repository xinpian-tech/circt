//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the WordCorrespondence pass: signal correspondence on
// words. The candidates are whole values of the word-level IR (registers and
// combinational operations) that are equal to each other or constant in
// every reachable state, and the bits of a value that are constant. They come
// from word-level random simulation (APInt) from the initial state, and are
// proved by induction as in van Eijk's method: they hold in the initial state
// for all inputs, and if all of them hold in a cycle they hold in the next.
// Both checks are bit-vector problems written as SMT-LIB from the word-level
// IR and solved by an external SMT solver (Bitwuzla by default); a model
// (counterexample) is simulated to split the classes, until no class splits.
// The proved classes are merged on the word-level IR, before bit-blasting.
//
// Operations the encoding does not know are free values in each cycle, which
// can only make the checks fail. Assumptions are not used: the classes hold
// on every trace.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Seq/SeqPasses.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Matchers.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include <chrono>
#include <sys/wait.h>
#include <unistd.h>
#include <map>
#include <random>

namespace circt {
namespace seq {
#define GEN_PASS_DEF_WORDCORRESPONDENCE
#include "circt/Dialect/Seq/SeqPasses.h.inc"
} // namespace seq
} // namespace circt

using namespace mlir;
using namespace circt;
using namespace seq;

namespace {

/// A node of the word-level net: an input, a register, a constant, an
/// operation the encoding knows, or a free value (anything else).
enum class Kind { Input, Register, Constant, Op, Free };

struct Node {
  Value value;
  Kind kind;
  unsigned width;
  SmallVector<unsigned> operands; // node indices (Op), next state (Register)
  std::optional<APInt> init;      // Register: constant initial value
};

/// A candidate class: members (node indices) equal to each other, or, if
/// `constant`, all equal to `value`.
struct Class {
  SmallVector<unsigned> members;
  bool constant = false;
  APInt value;
};

/// Bits of a node that are constant: `mask` of them, with values `bits`.
struct ConstBits {
  APInt mask, bits;
};

/// An SMT solver process, reading commands on its standard input and
/// answering on its standard output.
struct Session {
  pid_t pid = -1;
  int in = -1, out = -1;
  std::string buffer;
  bool start(StringRef program) {
    int toChild[2], fromChild[2];
    if (pipe(toChild) || pipe(fromChild))
      return false;
    pid = fork();
    if (pid < 0)
      return false;
    if (pid == 0) {
      dup2(toChild[0], 0);
      dup2(fromChild[1], 1);
      close(toChild[1]);
      close(fromChild[0]);
      std::string p = program.str();
      execl(p.c_str(), p.c_str(), "--lang", "smt2", (char *)nullptr);
      _exit(127);
    }
    close(toChild[0]);
    close(fromChild[1]);
    in = toChild[1];
    out = fromChild[0];
    return true;
  }
  std::unique_ptr<llvm::raw_fd_ostream> transcript;
  void write(StringRef text) {
    if (transcript)
      *transcript << text, transcript->flush();
    while (!text.empty()) {
      ssize_t n = ::write(in, text.data(), text.size());
      if (n <= 0)
        return;
      text = text.drop_front(n);
    }
  }
  bool fill() {
    char chunk[65536];
    ssize_t n = ::read(out, chunk, sizeof(chunk));
    if (n <= 0)
      return false;
    buffer.append(chunk, n);
    return true;
  }
  /// The next non-empty line.
  std::string readLine() {
    while (true) {
      size_t nl;
      while ((nl = buffer.find('\n')) == std::string::npos)
        if (!fill())
          return std::exchange(buffer, "");
      std::string line = buffer.substr(0, nl);
      buffer.erase(0, nl + 1);
      if (!StringRef(line).trim().empty())
        return line;
    }
  }
  /// One balanced s-expression.
  std::string readSexpr() {
    size_t pos = 0;
    int depth = 0;
    bool started = false;
    while (true) {
      for (; pos < buffer.size(); ++pos) {
        char c = buffer[pos];
        if (c == '(') {
          ++depth;
          started = true;
        } else if (c == ')') {
          --depth;
          if (started && depth == 0) {
            std::string e = buffer.substr(0, pos + 1);
            buffer.erase(0, pos + 1);
            return e;
          }
        }
      }
      if (!fill())
        return std::exchange(buffer, "");
    }
  }
  ~Session() {
    if (pid > 0) {
      write("(exit)\n");
      close(in);
      close(out);
      waitpid(pid, nullptr, 0);
    }
  }
};

struct Correspondence {
  hw::HWModuleOp module;
  unsigned patterns, frames;
  uint64_t seed;
  unsigned maxIterations;
  std::string solver;
  std::string dumpDir;

  SmallVector<Node> nodes;
  DenseMap<Value, unsigned> index;
  SmallVector<unsigned> inputs, regs, frees, order; // order: Op nodes, topo
  SmallVector<Class> classes;
  DenseMap<unsigned, ConstBits> constBits;
  unsigned numQueries = 0, numCex = 0, numCut = 0;
  std::unique_ptr<Session> session;
  bool failed = false;

  bool build();
  unsigned node(Value v);

  // Simulation of one pattern: values of all nodes in a cycle, from the
  // register values and the input and free values of the cycle.
  void eval(SmallVectorImpl<APInt> &vals) const;
  APInt evalOp(const Node &n, ArrayRef<APInt> vals) const;
  void simulate();

  // Splitting by observed values (one cycle of one pattern).
  void split(ArrayRef<APInt> vals);
  // Random continuation from the state of `vals`: patterns times frames
  // cycles, splitting in each.
  void continueFrom(ArrayRef<APInt> vals);
  std::mt19937_64 cexRng{1};
  unsigned cexPatterns = 16, cexFrames = 8;

  // SMT encoding.
  std::string smtValue(unsigned i, unsigned frame) const;
  void defineFrame(raw_ostream &os, unsigned frame) const;
  std::string candidates(unsigned frame) const;
  bool query(bool step);
  bool prove();

  std::pair<unsigned, unsigned> merge();
};

unsigned Correspondence::node(Value v) {
  auto it = index.find(v);
  if (it != index.end())
    return it->second;
  // A value of a type the net does not know, or defined elsewhere: free.
  unsigned i = nodes.size();
  unsigned width = 1;
  if (auto t = dyn_cast<IntegerType>(v.getType()))
    width = t.getWidth();
  nodes.push_back({v, Kind::Free, width, {}, std::nullopt});
  index[v] = i;
  frees.push_back(i);
  return i;
}

static bool knownOp(Operation *op) {
  return isa<comb::AndOp, comb::OrOp, comb::XorOp, comb::AddOp, comb::SubOp,
             comb::MulOp, comb::ICmpOp, comb::MuxOp, comb::ExtractOp,
             comb::ConcatOp, comb::ReplicateOp, comb::ShlOp, comb::ShrUOp,
             comb::ShrSOp>(op);
}

/// The constant initial value of a register, if it has one.
static std::optional<APInt> constantInitial(CompRegOp reg) {
  auto initial = reg.getInitialValue();
  if (!initial || !initial.getDefiningOp<InitialOp>())
    return std::nullopt;
  APInt value;
  if (!matchPattern(unwrapImmutableValue(initial), m_ConstantInt(&value)))
    return std::nullopt;
  return value;
}

bool Correspondence::build() {
  Block *body = module.getBodyBlock();
  // The clock of the registers: the most common one.
  DenseMap<Value, unsigned> clocks;
  for (auto reg : body->getOps<CompRegOp>())
    ++clocks[reg.getClk()];
  Value clock;
  unsigned best = 0;
  for (auto [c, n] : clocks)
    if (n > best)
      clock = c, best = n;

  auto add = [&](Value v, Kind kind) {
    unsigned width = cast<IntegerType>(v.getType()).getWidth();
    unsigned i = nodes.size();
    nodes.push_back({v, kind, width, {}, std::nullopt});
    index[v] = i;
    return i;
  };
  for (auto arg : body->getArguments())
    if (isa<IntegerType>(arg.getType()))
      inputs.push_back(add(arg, Kind::Input));
  SmallVector<Operation *> ops;
  for (auto &op : *body) {
    if (op.getNumResults() != 1 || !isa<IntegerType>(op.getResultTypes()[0]))
      continue;
    Value v = op.getResult(0);
    if (auto reg = dyn_cast<CompRegOp>(op)) {
      // Registers on another clock, with a reset, an inner symbol or a
      // non-constant initial value are free.
      bool ok = reg.getClk() == clock && !reg.getReset() &&
                !reg.getInnerSymAttr();
      std::optional<APInt> init;
      if (reg.getInitialValue()) {
        init = constantInitial(reg);
        ok &= init.has_value();
      }
      if (!ok)
        continue;
      unsigned i = add(v, Kind::Register);
      nodes[i].init = init;
      regs.push_back(i);
      ops.push_back(&op);
      continue;
    }
    if (auto c = dyn_cast<hw::ConstantOp>(op)) {
      add(v, Kind::Constant);
      continue;
    }
    if (knownOp(&op)) {
      add(v, Kind::Op);
      ops.push_back(&op);
    }
  }
  // Operands (free values for anything unknown).
  for (auto *op : ops) {
    unsigned i = index.lookup(op->getResult(0));
    if (auto reg = dyn_cast<CompRegOp>(op)) {
      unsigned next = node(reg.getInput());
      nodes[i].operands.push_back(next);
      continue;
    }
    for (auto operand : op->getOperands()) {
      unsigned j = node(operand);
      nodes[i].operands.push_back(j);
    }
  }
  // Topological order of the operations (registers, inputs, constants and
  // free values are leaves). Words can form cycles that their bits do not
  // (a field of a concatenation feeding another field): an operation closing
  // a cycle becomes a free value, which can only make the checks fail.
  while (true) {
    order.clear();
    SmallVector<uint8_t> state(nodes.size(), 0);
    SmallVector<unsigned> cut;
    for (unsigned root = 0; root < nodes.size(); ++root) {
      if (nodes[root].kind != Kind::Op || state[root])
        continue;
      SmallVector<std::pair<unsigned, unsigned>> stack{{root, 0}};
      state[root] = 1;
      while (!stack.empty()) {
        unsigned i = stack.back().first;
        unsigned k = stack.back().second++;
        if (k < nodes[i].operands.size()) {
          unsigned j = nodes[i].operands[k];
          if (nodes[j].kind != Kind::Op)
            continue;
          if (state[j] == 1) {
            cut.push_back(j);
            continue;
          }
          if (state[j] == 0) {
            state[j] = 1;
            stack.push_back({j, 0});
          }
          continue;
        }
        state[i] = 2;
        order.push_back(i);
        stack.pop_back();
      }
    }
    if (cut.empty())
      break;
    for (unsigned j : cut) {
      if (nodes[j].kind != Kind::Op)
        continue;
      nodes[j].kind = Kind::Free;
      nodes[j].operands.clear();
      frees.push_back(j);
      ++numCut;
    }
  }
  return true;
}

APInt Correspondence::evalOp(const Node &n, ArrayRef<APInt> vals) const {
  Operation *op = n.value.getDefiningOp();
  auto in = [&](unsigned k) -> const APInt & { return vals[n.operands[k]]; };
  unsigned numOperands = n.operands.size();
  if (isa<comb::AndOp>(op)) {
    APInt r = in(0);
    for (unsigned k = 1; k < numOperands; ++k)
      r &= in(k);
    return r;
  }
  if (isa<comb::OrOp>(op)) {
    APInt r = in(0);
    for (unsigned k = 1; k < numOperands; ++k)
      r |= in(k);
    return r;
  }
  if (isa<comb::XorOp>(op)) {
    APInt r = in(0);
    for (unsigned k = 1; k < numOperands; ++k)
      r ^= in(k);
    return r;
  }
  if (isa<comb::AddOp>(op)) {
    APInt r = in(0);
    for (unsigned k = 1; k < numOperands; ++k)
      r += in(k);
    return r;
  }
  if (isa<comb::MulOp>(op)) {
    APInt r = in(0);
    for (unsigned k = 1; k < numOperands; ++k)
      r *= in(k);
    return r;
  }
  if (isa<comb::SubOp>(op))
    return in(0) - in(1);
  if (auto icmp = dyn_cast<comb::ICmpOp>(op)) {
    const APInt &a = in(0), &b = in(1);
    bool r = false;
    switch (icmp.getPredicate()) {
    case comb::ICmpPredicate::eq:
    case comb::ICmpPredicate::ceq:
    case comb::ICmpPredicate::weq:
      r = a == b;
      break;
    case comb::ICmpPredicate::ne:
    case comb::ICmpPredicate::cne:
    case comb::ICmpPredicate::wne:
      r = a != b;
      break;
    case comb::ICmpPredicate::slt:
      r = a.slt(b);
      break;
    case comb::ICmpPredicate::sle:
      r = a.sle(b);
      break;
    case comb::ICmpPredicate::sgt:
      r = a.sgt(b);
      break;
    case comb::ICmpPredicate::sge:
      r = a.sge(b);
      break;
    case comb::ICmpPredicate::ult:
      r = a.ult(b);
      break;
    case comb::ICmpPredicate::ule:
      r = a.ule(b);
      break;
    case comb::ICmpPredicate::ugt:
      r = a.ugt(b);
      break;
    case comb::ICmpPredicate::uge:
      r = a.uge(b);
      break;
    }
    return APInt(1, r);
  }
  if (isa<comb::MuxOp>(op))
    return in(0).getBoolValue() ? in(1) : in(2);
  if (auto extract = dyn_cast<comb::ExtractOp>(op))
    return in(0).extractBits(n.width, extract.getLowBit());
  if (isa<comb::ConcatOp>(op)) {
    APInt r(n.width, 0);
    unsigned pos = n.width;
    for (unsigned k = 0; k < numOperands; ++k) {
      pos -= in(k).getBitWidth();
      r.insertBits(in(k), pos);
    }
    return r;
  }
  if (isa<comb::ReplicateOp>(op)) {
    APInt r(n.width, 0);
    unsigned w = in(0).getBitWidth();
    for (unsigned pos = 0; pos < n.width; pos += w)
      r.insertBits(in(0), pos);
    return r;
  }
  auto amount = [&]() -> unsigned {
    const APInt &b = in(1);
    return b.uge(n.width) ? n.width : b.getZExtValue();
  };
  if (isa<comb::ShlOp>(op))
    return in(0).shl(amount());
  if (isa<comb::ShrUOp>(op))
    return in(0).lshr(amount());
  if (isa<comb::ShrSOp>(op))
    return in(0).ashr(std::min(amount(), n.width - 1));
  llvm_unreachable("unknown op");
}

void Correspondence::eval(SmallVectorImpl<APInt> &vals) const {
  for (unsigned i : order)
    vals[i] = evalOp(nodes[i], vals);
}

/// Hash of the values of a node over all patterns and frames.
void Correspondence::simulate() {
  std::mt19937_64 rng(seed);
  auto random = [&](unsigned width) {
    SmallVector<uint64_t> words((width + 63) / 64);
    for (auto &w : words)
      w = rng();
    return APInt(width, words);
  };
  // Signatures: a hash of the values in every (pattern, frame), and the
  // constant bits (bits equal to their first value everywhere).
  SmallVector<uint64_t> sig(nodes.size(), 0);
  SmallVector<APInt> first(nodes.size()), same(nodes.size());
  bool started = false;
  SmallVector<APInt> vals(nodes.size());
  for (unsigned i = 0; i < nodes.size(); ++i)
    if (nodes[i].kind == Kind::Constant)
      vals[i] = cast<hw::ConstantOp>(nodes[i].value.getDefiningOp())
                    .getValue();
  for (unsigned p = 0; p < patterns; ++p) {
    for (unsigned r : regs)
      vals[r] = nodes[r].init ? *nodes[r].init : random(nodes[r].width);
    for (unsigned f = 0; f < frames; ++f) {
      for (unsigned i : inputs)
        vals[i] = random(nodes[i].width);
      for (unsigned i : frees)
        vals[i] = random(nodes[i].width);
      eval(vals);
      for (unsigned i = 0; i < nodes.size(); ++i) {
        if (nodes[i].kind != Kind::Op && nodes[i].kind != Kind::Register)
          continue;
        sig[i] = llvm::hash_combine(sig[i], llvm::hash_value(vals[i]));
        if (!started) {
          first[i] = vals[i];
          same[i] = APInt::getAllOnes(nodes[i].width);
        } else {
          same[i] &= ~(vals[i] ^ first[i]);
        }
      }
      started = true;
      // Next state.
      SmallVector<APInt> next;
      for (unsigned r : regs)
        next.push_back(vals[nodes[r].operands[0]]);
      for (auto [r, v] : llvm::zip(regs, next))
        vals[r] = v;
    }
  }
  // Classes: nodes of one width and signature; constants among them.
  std::map<std::pair<unsigned, uint64_t>, unsigned> bySig;
  for (unsigned i = 0; i < nodes.size(); ++i) {
    if (nodes[i].kind != Kind::Op && nodes[i].kind != Kind::Register)
      continue;
    if (same[i].isAllOnes()) {
      // Constant: one class per (width, value).
      auto key = std::make_pair(nodes[i].width,
                                (uint64_t)llvm::hash_value(first[i]) | 1);
      auto [it, inserted] = bySig.try_emplace(key, classes.size());
      if (inserted) {
        classes.emplace_back();
        classes.back().constant = true;
        classes.back().value = first[i];
      }
      classes[it->second].members.push_back(i);
      continue;
    }
    if (!same[i].isZero())
      constBits[i] = {same[i], first[i] & same[i]};
    auto key = std::make_pair(nodes[i].width, sig[i] & ~(uint64_t)1);
    auto [it, inserted] = bySig.try_emplace(key, classes.size());
    if (inserted)
      classes.emplace_back();
    classes[it->second].members.push_back(i);
  }
  // Drop singletons; order members (registers first, then operations in
  // topological order) so the representative never depends on a member.
  DenseMap<unsigned, unsigned> rank;
  unsigned n = 0;
  for (unsigned r : regs)
    rank[r] = n++;
  for (unsigned i : order)
    rank[i] = n++;
  SmallVector<Class> kept;
  for (auto &c : classes) {
    if (!c.constant && c.members.size() < 2)
      continue;
    llvm::sort(c.members,
               [&](unsigned a, unsigned b) { return rank[a] < rank[b]; });
    kept.push_back(std::move(c));
  }
  classes = std::move(kept);
}

void Correspondence::split(ArrayRef<APInt> vals) {
  SmallVector<Class> next;
  for (auto &c : classes) {
    if (c.constant) {
      Class same;
      same.constant = true;
      same.value = c.value;
      std::map<std::string, Class> rest;
      for (unsigned i : c.members) {
        if (vals[i] == c.value)
          same.members.push_back(i);
        else
          rest[toString(vals[i], 16, false)].members.push_back(i);
      }
      if (!same.members.empty())
        next.push_back(std::move(same));
      for (auto &[k, r] : rest)
        if (r.members.size() > 1)
          next.push_back(std::move(r));
      continue;
    }
    // Members keep their order within each part.
    std::map<std::string, unsigned> part;
    SmallVector<Class> parts;
    for (unsigned i : c.members) {
      auto [it, inserted] =
          part.try_emplace(toString(vals[i], 16, false), parts.size());
      if (inserted)
        parts.emplace_back();
      parts[it->second].members.push_back(i);
    }
    for (auto &p : parts)
      if (p.members.size() > 1)
        next.push_back(std::move(p));
  }
  classes = std::move(next);
  for (auto &[i, cb] : constBits)
    cb.mask &= ~(vals[i] ^ cb.bits);
  SmallVector<unsigned> gone;
  for (auto &[i, cb] : constBits) {
    if (cb.mask.isZero())
      gone.push_back(i);
    else
      cb.bits &= cb.mask;
  }
  for (unsigned i : gone)
    constBits.erase(i);
}

static std::string bvLiteral(const APInt &v) {
  std::string s = "#b";
  for (int k = v.getBitWidth() - 1; k >= 0; --k)
    s += v[k] ? '1' : '0';
  return s;
}

void Correspondence::continueFrom(ArrayRef<APInt> start) {
  auto random = [&](unsigned width) {
    SmallVector<uint64_t> words((width + 63) / 64);
    for (auto &w : words)
      w = cexRng();
    return APInt(width, words);
  };
  for (unsigned p = 0; p < cexPatterns; ++p) {
    SmallVector<APInt> vals(start.begin(), start.end());
    for (unsigned f = 0; f < cexFrames; ++f) {
      SmallVector<APInt> next;
      for (unsigned r : regs)
        next.push_back(vals[nodes[r].operands[0]]);
      for (auto [r, v] : llvm::zip(regs, next))
        vals[r] = v;
      for (unsigned i : inputs)
        vals[i] = random(nodes[i].width);
      for (unsigned i : frees)
        vals[i] = random(nodes[i].width);
      eval(vals);
      split(vals);
    }
  }
}

std::string Correspondence::smtValue(unsigned i, unsigned frame) const {
  const Node &n = nodes[i];
  switch (n.kind) {
  case Kind::Constant:
    return bvLiteral(
        cast<hw::ConstantOp>(n.value.getDefiningOp()).getValue());
  case Kind::Input:
    return "i" + std::to_string(frame) + "_" + std::to_string(i);
  case Kind::Free:
    return "u" + std::to_string(frame) + "_" + std::to_string(i);
  case Kind::Register:
    // The state of frame 0 is free (or the initial value); that of frame 1
    // is the next state of frame 0.
    return frame == 0 ? "s_" + std::to_string(i)
                      : smtValue(n.operands[0], 0);
  case Kind::Op:
    return "v" + std::to_string(frame) + "_" + std::to_string(i);
  }
  llvm_unreachable("kind");
}

void Correspondence::defineFrame(raw_ostream &os, unsigned frame) const {
  for (unsigned i : inputs)
    os << "(declare-const " << smtValue(i, frame) << " (_ BitVec "
       << nodes[i].width << "))\n";
  for (unsigned i : frees)
    os << "(declare-const " << smtValue(i, frame) << " (_ BitVec "
       << nodes[i].width << "))\n";
  for (unsigned i : order) {
    const Node &n = nodes[i];
    Operation *op = n.value.getDefiningOp();
    auto in = [&](unsigned k) { return smtValue(n.operands[k], frame); };
    auto fold = [&](StringRef f) {
      std::string e = in(0);
      for (unsigned k = 1; k < n.operands.size(); ++k)
        e = "(" + f.str() + " " + e + " " + in(k) + ")";
      return e;
    };
    std::string e;
    if (isa<comb::AndOp>(op))
      e = fold("bvand");
    else if (isa<comb::OrOp>(op))
      e = fold("bvor");
    else if (isa<comb::XorOp>(op))
      e = fold("bvxor");
    else if (isa<comb::AddOp>(op))
      e = fold("bvadd");
    else if (isa<comb::MulOp>(op))
      e = fold("bvmul");
    else if (isa<comb::SubOp>(op))
      e = "(bvsub " + in(0) + " " + in(1) + ")";
    else if (auto icmp = dyn_cast<comb::ICmpOp>(op)) {
      std::string c;
      switch (icmp.getPredicate()) {
      case comb::ICmpPredicate::eq:
      case comb::ICmpPredicate::ceq:
      case comb::ICmpPredicate::weq:
        c = "(= " + in(0) + " " + in(1) + ")";
        break;
      case comb::ICmpPredicate::ne:
      case comb::ICmpPredicate::cne:
      case comb::ICmpPredicate::wne:
        c = "(not (= " + in(0) + " " + in(1) + "))";
        break;
      case comb::ICmpPredicate::slt:
        c = "(bvslt " + in(0) + " " + in(1) + ")";
        break;
      case comb::ICmpPredicate::sle:
        c = "(bvsle " + in(0) + " " + in(1) + ")";
        break;
      case comb::ICmpPredicate::sgt:
        c = "(bvsgt " + in(0) + " " + in(1) + ")";
        break;
      case comb::ICmpPredicate::sge:
        c = "(bvsge " + in(0) + " " + in(1) + ")";
        break;
      case comb::ICmpPredicate::ult:
        c = "(bvult " + in(0) + " " + in(1) + ")";
        break;
      case comb::ICmpPredicate::ule:
        c = "(bvule " + in(0) + " " + in(1) + ")";
        break;
      case comb::ICmpPredicate::ugt:
        c = "(bvugt " + in(0) + " " + in(1) + ")";
        break;
      case comb::ICmpPredicate::uge:
        c = "(bvuge " + in(0) + " " + in(1) + ")";
        break;
      }
      e = "(ite " + c + " #b1 #b0)";
    } else if (isa<comb::MuxOp>(op))
      e = "(ite (= " + in(0) + " #b1) " + in(1) + " " + in(2) + ")";
    else if (auto extract = dyn_cast<comb::ExtractOp>(op))
      e = "((_ extract " + std::to_string(extract.getLowBit() + n.width - 1) +
          " " + std::to_string(extract.getLowBit()) + ") " + in(0) + ")";
    else if (isa<comb::ConcatOp>(op))
      e = fold("concat");
    else if (isa<comb::ReplicateOp>(op))
      e = "((_ repeat " +
          std::to_string(n.width / nodes[n.operands[0]].width) + ") " +
          in(0) + ")";
    else if (isa<comb::ShlOp>(op))
      e = "(bvshl " + in(0) + " " + in(1) + ")";
    else if (isa<comb::ShrUOp>(op))
      e = "(bvlshr " + in(0) + " " + in(1) + ")";
    else if (isa<comb::ShrSOp>(op))
      e = "(bvashr " + in(0) + " " + in(1) + ")";
    os << "(define-fun " << smtValue(i, frame) << " () (_ BitVec " << n.width
       << ") " << e << ")\n";
  }
}

/// The conjunction of all candidates in a frame.
std::string Correspondence::candidates(unsigned frame) const {
  std::string s;
  llvm::raw_string_ostream os(s);
  os << "(and true";
  for (auto &c : classes) {
    if (c.constant) {
      for (unsigned i : c.members)
        os << " (= " << smtValue(i, frame) << " " << bvLiteral(c.value) << ")";
      continue;
    }
    for (unsigned i : llvm::drop_begin(c.members))
      os << " (= " << smtValue(i, frame) << " "
         << smtValue(c.members[0], frame) << ")";
  }
  for (auto &[i, cb] : constBits)
    os << " (= (bvand " << smtValue(i, frame) << " " << bvLiteral(cb.mask)
       << ") " << bvLiteral(cb.bits) << ")";
  os << ")";
  return s;
}

/// Parses `(name #b...)` pairs of a get-value answer.
static std::map<std::string, APInt> parseModel(StringRef text) {
  std::map<std::string, APInt> model;
  size_t pos = 0;
  while ((pos = text.find("#b", pos)) != StringRef::npos) {
    size_t end = pos + 2;
    while (end < text.size() && (text[end] == '0' || text[end] == '1'))
      ++end;
    StringRef bits = text.slice(pos + 2, end);
    // The name: the token before the literal, after the opening parenthesis.
    size_t nameEnd = pos;
    while (nameEnd > 0 && isspace(text[nameEnd - 1]))
      --nameEnd;
    size_t nameBegin = nameEnd;
    while (nameBegin > 0 && !isspace(text[nameBegin - 1]) &&
           text[nameBegin - 1] != '(')
      --nameBegin;
    StringRef name = text.slice(nameBegin, nameEnd);
    if (!name.empty() && !bits.empty())
      model.emplace(name.str(), APInt(bits.size(), bits, 2));
    pos = end;
  }
  return model;
}

/// One check: the base case (frame 0 from the initial state) or the
/// inductive step (frame 0 free, the candidates assumed there, checked in
/// frame 1). Returns true if the check holds; on a counterexample, splits the
/// classes and returns false.
bool Correspondence::query(bool step) {
  ++numQueries;
  if (getenv("WORD_CORRESPONDENCE_PROGRESS")) {
    size_t m = 0;
    for (auto &c : classes)
      m += c.members.size();
    llvm::errs() << "word-correspondence: query " << numQueries
                 << (step ? " step" : " base") << ", " << classes.size()
                 << " classes, " << m << " members, " << constBits.size()
                 << " with constant bits\n";
  }
  std::string q;
  {
    llvm::raw_string_ostream os(q);
    os << "(push 1)\n";
    if (step) {
      os << "(assert " << candidates(0) << ")\n";
      os << "(assert (not " << candidates(1) << "))\n";
    } else {
      os << "(assert init)\n";
      os << "(assert (not " << candidates(0) << "))\n";
    }
    os << "(check-sat)\n";
  }
  if (!dumpDir.empty()) {
    SmallString<128> copy(dumpDir);
    llvm::sys::path::append(copy, "query" + std::to_string(numQueries) +
                                      (step ? "-step" : "-base") + ".smt2");
    std::error_code ec;
    llvm::raw_fd_ostream dump(copy, ec);
    dump << q;
  }
  session->write(q);
  std::string answer = session->readLine();
  std::string text = answer;
  if (StringRef(answer).starts_with("sat")) {
    std::string get;
    llvm::raw_string_ostream os(get);
    os << "(get-value (";
    for (unsigned r : regs)
      os << " s_" << r;
    for (unsigned f = 0; f <= (step ? 1u : 0u); ++f) {
      for (unsigned i : inputs)
        os << " " << smtValue(i, f);
      for (unsigned i : frees)
        os << " " << smtValue(i, f);
    }
    // Debugging: the values of the operations, to find where the
    // simulation and the encoding disagree.
    if (!dumpDir.empty())
      for (unsigned f = 0; f <= (step ? 1u : 0u); ++f)
        for (unsigned i : order)
          os << " " << smtValue(i, f);
    os << "))\n";
    session->write(get);
    text += session->readSexpr();
  }
  session->write("(pop 1)\n");
  if (StringRef(text).starts_with("unsat"))
    return true;
  if (!StringRef(text).starts_with("sat")) {
    module.emitError("unexpected SMT solver answer: ")
        << StringRef(text).take_front(200);
    failed = true;
    return true;
  }
  ++numCex;
  auto model = parseModel(StringRef(text).drop_front(3));
  auto get = [&](const std::string &name, unsigned width) {
    auto it = model.find(name);
    return it != model.end() && it->second.getBitWidth() == width
               ? it->second
               : APInt(width, 0);
  };
  // Simulate the counterexample and split by the values of its last frame.
  SmallVector<APInt> vals(nodes.size()), vals0;
  for (unsigned i = 0; i < nodes.size(); ++i)
    if (nodes[i].kind == Kind::Constant)
      vals[i] = cast<hw::ConstantOp>(nodes[i].value.getDefiningOp())
                    .getValue();
  for (unsigned r : regs)
    vals[r] = get("s_" + std::to_string(r), nodes[r].width);
  for (unsigned f = 0; f <= (step ? 1u : 0u); ++f) {
    if (f == 1) {
      SmallVector<APInt> next;
      for (unsigned r : regs)
        next.push_back(vals[nodes[r].operands[0]]);
      for (auto [r, v] : llvm::zip(regs, next))
        vals[r] = v;
    }
    for (unsigned i : inputs)
      vals[i] = get(smtValue(i, f), nodes[i].width);
    for (unsigned i : frees)
      vals[i] = get(smtValue(i, f), nodes[i].width);
    eval(vals);
    if (f == 0)
      vals0 = vals;
  }
  // A measure that decreases with every split: classes count up, members
  // and constant bits down.
  auto measure = [&]() {
    size_t m = 0;
    for (auto &c : classes)
      m += c.members.size();
    size_t bits = 0;
    for (auto &[i, cb] : constBits)
      bits += cb.mask.popcount();
    return std::make_tuple(classes.size(), m, bits);
  };
  auto before = measure();
  split(vals);
  bool progress = measure() != before;
  continueFrom(vals);
  if (!progress) {
    // The counterexample does not split anything: the simulation disagrees
    // with the encoding. Give up rather than loop.
    module.emitWarning("word correspondence: counterexample splits nothing");
    auto &vals1 = vals;
    for (unsigned f = 0; f <= (step ? 1u : 0u); ++f)
    for (unsigned i : order) {
      auto &vals = f == 0 ? vals0 : vals1;
      auto it = model.find(smtValue(i, f));
      if (it == model.end() || it->second == vals[i])
        continue;
      auto d = nodes[i].value.getDefiningOp()->emitRemark(
          "simulation and encoding disagree: simulated ");
      d << toString(vals[i], 16, false) << ", solver "
        << toString(it->second, 16, false) << "; operands:";
      for (unsigned j : nodes[i].operands) {
        auto jt = model.find(smtValue(j, f));
        d << " " << toString(vals[j], 16, false) << "/"
          << (jt != model.end() ? toString(jt->second, 16, false) : "?");
      }
      d << " (frame " << f << ")";
      f = 2;
      break;
    }
    failed = true;
    return true;
  }
  return false;
}

bool Correspondence::prove() {
  auto program = llvm::sys::findProgramByName(solver);
  if (!program) {
    module.emitError("cannot find SMT solver ") << solver;
    return false;
  }
  session = std::make_unique<Session>();
  if (!dumpDir.empty()) {
    SmallString<128> path(dumpDir);
    llvm::sys::path::append(path, "session.smt2");
    std::error_code ec;
    session->transcript = std::make_unique<llvm::raw_fd_ostream>(path, ec);
  }
  if (!session->start(*program)) {
    module.emitError("cannot start SMT solver ") << solver;
    return false;
  }
  {
    std::string header;
    llvm::raw_string_ostream os(header);
    os << "(set-logic QF_BV)\n(set-option :produce-models true)\n";
    for (unsigned r : regs)
      os << "(declare-const s_" << r << " (_ BitVec " << nodes[r].width
         << "))\n";
    defineFrame(os, 0);
    defineFrame(os, 1);
    os << "(define-fun init () Bool (and true";
    for (unsigned r : regs)
      if (nodes[r].init)
        os << " (= s_" << r << " " << bvLiteral(*nodes[r].init) << ")";
    os << "))\n";
    session->write(header);
  }
  for (unsigned it = 0; it < maxIterations; ++it) {
    bool base = query(/*step=*/false);
    if (failed)
      return false;
    if (!base)
      continue;
    bool step = query(/*step=*/true);
    if (failed)
      return false;
    if (step)
      return true;
  }
  return false;
}

std::pair<unsigned, unsigned> Correspondence::merge() {
  OpBuilder builder(module.getContext());
  builder.setInsertionPoint(module.getBodyBlock()->getTerminator());
  unsigned values = 0, bits = 0;
  DenseSet<unsigned> replaced;
  for (auto &c : classes) {
    if (c.constant) {
      Value k;
      for (unsigned i : c.members) {
        Value v = nodes[i].value;
        if (!k)
          k = hw::ConstantOp::create(builder, v.getLoc(), c.value);
        v.replaceAllUsesWith(k);
        replaced.insert(i);
        ++values;
      }
      continue;
    }
    Value repr = nodes[c.members[0]].value;
    for (unsigned i : llvm::drop_begin(c.members)) {
      nodes[i].value.replaceAllUsesWith(repr);
      replaced.insert(i);
      ++values;
    }
  }
  // Constant bits: the value becomes a concatenation of its other bits and
  // the constants.
  for (auto &[i, cb] : constBits) {
    if (replaced.contains(i))
      continue;
    Value v = nodes[i].value;
    unsigned width = nodes[i].width;
    builder.setInsertionPointAfterValue(v);
    SmallVector<Value> parts;
    SmallVector<Operation *> created;
    for (int hi = width; hi > 0;) {
      bool isConst = cb.mask[hi - 1];
      int lo = hi - 1;
      while (lo > 0 && cb.mask[lo - 1] == isConst)
        --lo;
      unsigned len = hi - lo;
      if (isConst) {
        auto c = hw::ConstantOp::create(builder, v.getLoc(),
                                        cb.bits.extractBits(len, lo));
        parts.push_back(c);
      } else {
        auto e = comb::ExtractOp::create(builder, v.getLoc(), v, lo, len);
        parts.push_back(e);
        created.push_back(e);
      }
      hi = lo;
    }
    auto concat = comb::ConcatOp::create(builder, v.getLoc(), parts);
    v.replaceAllUsesExcept(concat, SmallPtrSet<Operation *, 8>(
                                       created.begin(), created.end()));
    bits += cb.mask.popcount();
  }
  return {values, bits};
}

struct WordCorrespondencePass
    : public seq::impl::WordCorrespondenceBase<WordCorrespondencePass> {
  using WordCorrespondenceBase::WordCorrespondenceBase;
  void runOnOperation() override {
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    auto lap = [&](auto &stat) {
      auto now = Clock::now();
      stat +=
          std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
              .count();
      start = now;
    };
    Correspondence c;
    c.module = getOperation();
    c.patterns = numPatterns;
    c.frames = numFrames;
    c.seed = seed;
    c.maxIterations = maxIterations;
    c.solver = solver;
    c.dumpDir = dumpDir;
    if (!c.build()) {
      getOperation().emitWarning("word correspondence: combinational cycle");
      return markAllAnalysesPreserved();
    }
    numCutOps += c.numCut;
    c.simulate();
    lap(msSimulate);
    for (auto &cl : c.classes)
      numCandidates += cl.members.size();
    numCandidateBits += c.constBits.size();
    bool proved = c.prove();
    lap(msProve);
    numQueries += c.numQueries;
    numCounterexamples += c.numCex;
    if (!proved) {
      getOperation().emitWarning("word correspondence gave up after ")
          << c.numQueries << " queries; nothing merged";
      return markAllAnalysesPreserved();
    }
    auto [values, bits] = c.merge();
    numMergedValues += values;
    numConstantBits += bits;
    numClasses += c.classes.size();
  }
};

} // namespace
