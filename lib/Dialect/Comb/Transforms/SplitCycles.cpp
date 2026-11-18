//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the SplitCycles pass: combinational cycles of words
// that their bits do not have (a field of a concatenation feeding another
// field of it, as aggregates lowered to integers often give) are removed by
// splitting the operations on the cycle into bits. Only bitwise operations
// (and, or, xor), muxes, concatenations, extracts and replications are
// split; their bit k depends only on bit k (or a known bit) of their
// operands. Values used outside the cycle are rebuilt as concatenations of
// their bits. A cycle through any other operation is left alone.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/Comb/CombPasses.h"
#include "circt/Dialect/HW/HWOps.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/SmallVector.h"
#include <map>

namespace circt {
namespace comb {
#define GEN_PASS_DEF_SPLITCYCLES
#include "circt/Dialect/Comb/Passes.h.inc"
} // namespace comb
} // namespace circt

using namespace mlir;
using namespace circt;
using namespace comb;

namespace {
struct SplitCyclesPass
    : public comb::impl::SplitCyclesBase<SplitCyclesPass> {
  void runOnOperation() override;
};
} // namespace

static bool splittable(Operation *op) {
  return isa<AndOp, OrOp, XorOp, MuxOp, ConcatOp, ExtractOp, ReplicateOp>(
             op) &&
         op->getNumResults() == 1 && isa<IntegerType>(op->getResultTypes()[0]);
}

/// Strongly connected components of the combinational operations (Tarjan).
static SmallVector<SmallVector<Operation *>> cycles(Block *body) {
  DenseMap<Operation *, unsigned> index, low;
  DenseSet<Operation *> onStack;
  SmallVector<Operation *> stack;
  SmallVector<SmallVector<Operation *>> result;
  unsigned next = 0;
  auto comb = [&](Value v) -> Operation * {
    Operation *op = v.getDefiningOp();
    if (!op || op->getBlock() != body ||
        !isa<CombDialect>(op->getDialect()))
      return nullptr;
    return op;
  };
  for (auto &root : *body) {
    if (!isa<CombDialect>(root.getDialect()) || index.count(&root))
      continue;
    // Iterative Tarjan: (op, next operand).
    SmallVector<std::pair<Operation *, unsigned>> work{{&root, 0}};
    index[&root] = low[&root] = next++;
    stack.push_back(&root);
    onStack.insert(&root);
    while (!work.empty()) {
      auto [op, k] = work.back();
      if (k < op->getNumOperands()) {
        ++work.back().second;
        Operation *dep = comb(op->getOperand(k));
        if (!dep)
          continue;
        if (!index.count(dep)) {
          index[dep] = low[dep] = next++;
          stack.push_back(dep);
          onStack.insert(dep);
          work.push_back({dep, 0});
        } else if (onStack.contains(dep)) {
          low[op] = std::min(low[op], index[dep]);
        }
        continue;
      }
      work.pop_back();
      if (!work.empty())
        low[work.back().first] = std::min(low[work.back().first], low[op]);
      if (low[op] == index[op]) {
        SmallVector<Operation *> scc;
        Operation *member;
        do {
          member = stack.pop_back_val();
          onStack.erase(member);
          scc.push_back(member);
        } while (member != op);
        bool selfLoop = llvm::any_of(op->getOperands(), [&](Value v) {
          return v.getDefiningOp() == op;
        });
        if (scc.size() > 1 || selfLoop)
          result.push_back(std::move(scc));
      }
    }
  }
  return result;
}

void SplitCyclesPass::runOnOperation() {
  Block *body = getOperation().getBodyBlock();
  auto sccs = cycles(body);
  unsigned splitOps = 0, kept = 0;
  for (auto &scc : sccs) {
    // Other operations (comparisons, arithmetic) stay whole: their operands
    // are rebuilt from bits, their result bits extracted. That is enough
    // when the cycle does not go through them bit by bit.
    if (!llvm::all_of(scc, [](Operation *op) {
          return op->getNumResults() == 1 &&
                 isa<IntegerType>(op->getResultTypes()[0]) &&
                 op->getNumRegions() == 0;
        })) {
      ++kept;
      continue;
    }
    DenseSet<Operation *> inScc(scc.begin(), scc.end());
    OpBuilder builder(getOperation().getContext());
    builder.setInsertionPoint(body->getTerminator());
    Location loc = scc.front()->getLoc();
    // Bits of every value of the cycle: placeholders first (the cycle has
    // no order), filled in below.
    DenseMap<Value, SmallVector<Value>> bits;
    auto placeholder = hw::ConstantOp::create(builder, loc, APInt(1, 0));
    for (auto *op : scc) {
      Value v = op->getResult(0);
      unsigned w = cast<IntegerType>(v.getType()).getWidth();
      bits[v].assign(w, Value());
    }
    // Bits of an operand: of the cycle, or extracts of an outside value.
    DenseMap<Value, SmallVector<Value>> outside;
    auto bitOf = [&](Value v, unsigned k) -> Value {
      Operation *def = v.getDefiningOp();
      if (def && inScc.contains(def))
        return Value(); // filled later
      auto &vec = outside[v];
      if (vec.empty()) {
        unsigned w = cast<IntegerType>(v.getType()).getWidth();
        vec.resize(w);
        for (unsigned i = 0; i < w; ++i)
          vec[i] = w == 1 ? v
                          : builder.createOrFold<ExtractOp>(loc, v, i, 1);
      }
      return vec[k];
    };
    // Each bit as an operation whose operands are resolved in a second step:
    // (bit op, operand index) -> (source value, source bit).
    struct Pending {
      Operation *op;
      unsigned operand;
      Value source;
      unsigned bit;
    };
    SmallVector<Pending> pending;
    // A bit that is just another bit (wiring): resolved by following.
    DenseMap<std::pair<Value, unsigned>, std::pair<Value, unsigned>> alias;
    SmallVector<std::pair<Operation *, Operation *>> wholeOps; // (old, new)
    for (auto *op : scc) {
      Value v = op->getResult(0);
      unsigned w = bits[v].size();
      if (!splittable(op)) {
        Operation *copy = builder.clone(*op);
        wholeOps.push_back({op, copy});
        for (unsigned k = 0; k < w; ++k)
          bits[v][k] = w == 1 ? copy->getResult(0)
                              : builder.createOrFold<ExtractOp>(
                                    loc, copy->getResult(0), k, 1);
        continue;
      }
      for (unsigned k = 0; k < w; ++k) {
        auto use = [&](Operation *bitOp, unsigned idx, Value src,
                       unsigned b) {
          if (Value outsideBit = bitOf(src, b))
            bitOp->setOperand(idx, outsideBit);
          else
            pending.push_back({bitOp, idx, src, b});
        };
        if (isa<AndOp, OrOp, XorOp>(op)) {
          SmallVector<Value> operands(op->getNumOperands(), placeholder);
          Operation *bitOp;
          if (isa<AndOp>(op))
            bitOp = AndOp::create(builder, loc, operands, false);
          else if (isa<OrOp>(op))
            bitOp = OrOp::create(builder, loc, operands, false);
          else
            bitOp = XorOp::create(builder, loc, operands, false);
          for (unsigned i = 0; i < op->getNumOperands(); ++i)
            use(bitOp, i, op->getOperand(i), k);
          bits[v][k] = bitOp->getResult(0);
        } else if (auto mux = dyn_cast<MuxOp>(op)) {
          auto bitOp = MuxOp::create(builder, loc, placeholder, placeholder,
                                     placeholder, false);
          use(bitOp, 0, mux.getCond(), 0);
          use(bitOp, 1, mux.getTrueValue(), k);
          use(bitOp, 2, mux.getFalseValue(), k);
          bits[v][k] = bitOp.getResult();
        } else if (auto extract = dyn_cast<ExtractOp>(op)) {
          alias[{v, k}] = {extract.getInput(), extract.getLowBit() + k};
        } else if (auto rep = dyn_cast<ReplicateOp>(op)) {
          unsigned iw = cast<IntegerType>(rep.getInput().getType()).getWidth();
          alias[{v, k}] = {rep.getInput(), k % iw};
        } else if (auto concat = dyn_cast<ConcatOp>(op)) {
          unsigned pos = 0;
          for (auto input : llvm::reverse(concat.getInputs())) {
            unsigned iw = cast<IntegerType>(input.getType()).getWidth();
            if (k < pos + iw) {
              alias[{v, k}] = {input, k - pos};
              break;
            }
            pos += iw;
          }
        }
      }
    }
    // Resolve a bit through wiring to an operation bit or an outside bit.
    std::function<Value(Value, unsigned, unsigned)> resolve =
        [&](Value v, unsigned k, unsigned depth) -> Value {
      if (Value o = bitOf(v, k))
        return o;
      if (depth > 100000)
        return Value();
      auto it = alias.find({v, k});
      if (it != alias.end())
        return resolve(it->second.first, it->second.second, depth + 1);
      return bits[v][k];
    };
    bool ok = true;
    for (auto &p : pending) {
      Value b = resolve(p.source, p.bit, 0);
      if (!b) {
        ok = false;
        break;
      }
      p.op->setOperand(p.operand, b);
    }
    // Operands of the whole operations, rebuilt from bits.
    for (auto [op, copy] : wholeOps) {
      if (!ok)
        break;
      for (unsigned i = 0; i < op->getNumOperands(); ++i) {
        Value src = op->getOperand(i);
        Operation *def = src.getDefiningOp();
        if (!def || !inScc.contains(def))
          continue;
        unsigned w = cast<IntegerType>(src.getType()).getWidth();
        SmallVector<Value> msbFirst;
        for (int k = w - 1; k >= 0; --k) {
          Value bit = resolve(src, k, 0);
          if (!bit) {
            ok = false;
            break;
          }
          msbFirst.push_back(bit);
        }
        if (!ok)
          break;
        copy->setOperand(i, w == 1 ? msbFirst[0]
                                   : ConcatOp::create(builder, loc, msbFirst)
                                         .getResult());
      }
    }
    // Rebuild the values for their users outside the cycle.
    if (ok) {
      for (auto *op : scc) {
        Value v = op->getResult(0);
        unsigned w = bits[v].size();
        SmallVector<Value> msbFirst;
        for (int k = w - 1; k >= 0; --k) {
          Value b = resolve(v, k, 0);
          if (!b) {
            ok = false;
            break;
          }
          msbFirst.push_back(b);
        }
        if (!ok)
          break;
        Value whole = w == 1 ? msbFirst[0]
                             : ConcatOp::create(builder, loc, msbFirst)
                                   .getResult();
        v.replaceUsesWithIf(whole, [&](OpOperand &use) {
          return !inScc.contains(use.getOwner());
        });
      }
    }
    if (!ok) {
      // A cycle of wiring only (a bit equal to itself): leave it.
      ++kept;
      continue;
    }
    // The cycle's operations are now unused outside it.
    for (auto *op : scc)
      op->dropAllReferences();
    for (auto *op : scc)
      op->erase();
    splitOps += scc.size();
  }
  numSplitOps += splitOps;
  numKeptCycles += kept;
}
