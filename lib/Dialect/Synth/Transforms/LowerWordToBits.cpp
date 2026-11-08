//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass implements bit-blasting for logic synthesis operations.
// It converts multi-bit operations (AIG, combinatorial) into equivalent
// single-bit operations, enabling more efficient synthesis and optimization.
//
// Word-level combinational cycles are allowed: a word may depend on itself
// although none of its bits does (e.g. one field of a struct computed from
// another). When the bits of a word are needed while the word is being
// lowered, placeholders stand in for them and are replaced once the word is
// done. Cycles of bits are kept as they are.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Synth/SynthOps.h"
#include "circt/Dialect/Synth/Transforms/SynthPasses.h"
#include "circt/Support/LLVM.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/Value.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Support/LogicalResult.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <array>
#include <optional>

#define DEBUG_TYPE "synth-lower-word-to-bits"

namespace circt {
namespace synth {
#define GEN_PASS_DEF_LOWERWORDTOBITS
#include "circt/Dialect/Synth/Transforms/SynthPasses.h.inc"
} // namespace synth
} // namespace circt

using namespace circt;
using namespace synth;

//===----------------------------------------------------------------------===//
// Utilities
//===----------------------------------------------------------------------===//

/// Check if an operation should be lowered to bit-level operations.
static bool shouldLowerOperation(Operation *op) {
  // Lower all Synth boolean ops through the interface instead of maintaining a
  // per-op allowlist here.
  return isa<ChoiceOp, BooleanLogicOpInterface, comb::AndOp, comb::OrOp,
             comb::XorOp, comb::MuxOp>(op);
}

namespace {

//===----------------------------------------------------------------------===//
// BitBlaster - Bit-level lowering implementation
//===----------------------------------------------------------------------===//

/// The BitBlaster class implements the core bit-blasting algorithm.
/// It manages the lowering of multi-bit operations to single-bit operations
/// while maintaining correctness and optimizing for constant propagation.
class BitBlaster {
public:
  explicit BitBlaster(Operation *topOp) : topOp(topOp) {}

  /// Run the bit-blasting algorithm on the module.
  LogicalResult run();

  //===--------------------------------------------------------------------===//
  // Statistics
  //===--------------------------------------------------------------------===//

  /// Number of bits that were lowered from multi-bit to single-bit operations
  size_t numLoweredBits = 0;

  /// Number of constant bits that were identified and optimized
  size_t numLoweredConstants = 0;

  /// Number of operations that were lowered
  size_t numLoweredOps = 0;

private:
  //===--------------------------------------------------------------------===//
  // Core Lowering Methods
  //===--------------------------------------------------------------------===//

  /// Lower a multi-bit value to individual bits.
  /// This is the main entry point for bit-blasting a value.
  ArrayRef<Value> lowerValueToBits(Value value);
  ArrayRef<Value> lowerBooleanLogicOperation(BooleanLogicOpInterface op);
  template <typename OpTy>
  ArrayRef<Value> lowerCombLogicOperations(OpTy op);
  ArrayRef<Value> lowerCombMux(comb::MuxOp op);
  ArrayRef<Value>
  lowerOp(Operation *op,
          llvm::function_ref<Value(OpBuilder &builder, ValueRange)> createOp);

  /// Extract a specific bit from a value.
  /// Handles various IR constructs that can represent bit extraction.
  Value extractBit(Value value, size_t index);

  /// Compute and cache known bits for a value.
  /// Uses operation-specific logic to determine which bits are constants.
  const llvm::KnownBits &computeKnownBits(Value value);

  /// Get or create a boolean constant (0 or 1).
  /// Constants are cached by block to avoid duplication.
  Value getBoolConstant(bool value, Block *block);

  //===--------------------------------------------------------------------===//
  // Helper Methods
  //===--------------------------------------------------------------------===//

  /// Insert lowered bits into the cache.
  ArrayRef<Value> insertBits(Value value, SmallVector<Value> bits) {
    auto it = loweredValues.insert({value, std::move(bits)});
    assert(it.second && "value already inserted");
    return it.first->second;
  }

  /// Insert computed known bits into the cache.
  const llvm::KnownBits &insertKnownBits(Value value, llvm::KnownBits bits) {
    auto it = knownBits.insert({value, std::move(bits)});
    return it.first->second;
  }

  /// Replace the placeholders by the bits they stand for.
  LogicalResult resolvePlaceholders();

  /// Cache for lowered values (multi-bit -> vector of single-bit values)
  llvm::MapVector<Value, SmallVector<Value>> loweredValues;

  /// Values being lowered (on the recursion stack), and the placeholder bits
  /// handed out for them while they are.
  DenseSet<Value> inProgress;
  llvm::MapVector<Value, SmallVector<Value>> placeholderBits;
  /// Placeholder -> (value, bit index) it stands for.
  DenseMap<Value, std::pair<Value, unsigned>> placeholders;

  /// (value, bit index) pairs being extracted through concat/extract/
  /// replicate, with the lowering depth at which they were reached, to detect
  /// cycles made only of wiring (a cycle through a lowered op ends at its
  /// placeholders instead).
  DenseMap<std::pair<Value, size_t>, unsigned> extracting;
  unsigned depth = 0;
  bool wiringCycle = false;

  /// Cache for computed known bits information
  llvm::MapVector<Value, llvm::KnownBits> knownBits;

  /// Cached boolean constants by block (false at index 0, true at index 1)
  llvm::DenseMap<Block *, std::array<Value, 2>> constantsByBlock;

  /// Reference to the top-level operation being processed
  Operation *topOp;
};

} // namespace

//===----------------------------------------------------------------------===//
// BitBlaster Implementation
//===----------------------------------------------------------------------===//

const llvm::KnownBits &BitBlaster::computeKnownBits(Value value) {
  // Check cache first
  auto *it = knownBits.find(value);
  if (it != knownBits.end())
    return it->second;

  auto width = hw::getBitWidth(value.getType());
  auto *op = value.getDefiningOp();

  // For block arguments, return unknown bits
  if (!op)
    return insertKnownBits(value, llvm::KnownBits(width));

  // A value that depends on itself is taken as unknown while its known bits
  // are computed, which can only lose known bits.
  insertKnownBits(value, llvm::KnownBits(width));

  llvm::KnownBits result(width);
  if (auto logicOp = dyn_cast<BooleanLogicOpInterface>(op))
    result =
        logicOp.computeKnownBits([&](unsigned i) -> const llvm::KnownBits & {
          return computeKnownBits(logicOp.getInput(i));
        });
  else if (auto choice = dyn_cast<ChoiceOp>(op)) {
    result = computeKnownBits(choice.getInputs().front());
    for (auto input : choice.getInputs().drop_front()) {
      auto known = computeKnownBits(input);
      result.One |= known.One;
      result.Zero |= known.Zero;
    }
  } else {
    // For other operations, use the standard known bits computation
    // TODO: This is not optimal as it has a depth limit and does not check
    // cached results.
    result = comb::computeKnownBits(value);
  }

  auto &entry = knownBits[value];
  entry = std::move(result);
  return entry;
}

Value BitBlaster::extractBit(Value value, size_t index) {
  if (hw::getBitWidth(value.getType()) <= 1) {
    // Look through single-bit extracts of lowered words, so that a bit read
    // back from a word on a cycle does not keep the word itself.
    if (auto ext = value.getDefiningOp<comb::ExtractOp>())
      if (auto *def = ext.getInput().getDefiningOp();
          def && shouldLowerOperation(def))
        return extractBit(ext.getInput(), ext.getLowBit());
    return value;
  }

  auto *op = value.getDefiningOp();

  // If the value is a block argument, extract the bit.
  if (!op)
    return lowerValueToBits(value)[index];

  std::pair<Value, size_t> key{value, index};
  std::optional<unsigned> outerDepth;
  bool wiring = isa<comb::ConcatOp, comb::ExtractOp, comb::ReplicateOp>(op);
  if (wiring) {
    auto [it, inserted] = extracting.insert({key, depth});
    if (!inserted) {
      if (it->second == depth) {
        // The bit is its own driver through wiring only.
        if (!wiringCycle)
          mlir::emitError(value.getLoc(), "bit ")
              << index << " of this value is driven by itself";
        wiringCycle = true;
        return getBoolConstant(false, op->getBlock());
      }
      outerDepth = it->second;
      it->second = depth;
    }
  }
  auto bit = TypeSwitch<Operation *, Value>(op)
      .Case<comb::ConcatOp>([&](comb::ConcatOp op) {
        for (auto operand : llvm::reverse(op.getOperands())) {
          auto width = hw::getBitWidth(operand.getType());
          assert(width >= 0 && "operand has zero width");
          if (index < static_cast<size_t>(width))
            return extractBit(operand, index);
          index -= static_cast<size_t>(width);
        }
        llvm_unreachable("index out of bounds");
      })
      .Case<comb::ExtractOp>([&](comb::ExtractOp ext) {
        return extractBit(ext.getInput(),
                          static_cast<size_t>(ext.getLowBit()) + index);
      })
      .Case<comb::ReplicateOp>([&](comb::ReplicateOp op) {
        return extractBit(op.getInput(),
                          index % static_cast<size_t>(hw::getBitWidth(
                                      op.getOperand().getType())));
      })
      .Case<hw::ConstantOp>([&](hw::ConstantOp op) {
        auto value = op.getValue();
        return getBoolConstant(value[index], op->getBlock());
      })
      .Default([&](auto op) { return lowerValueToBits(value)[index]; });
  if (wiring) {
    if (outerDepth)
      extracting[key] = *outerDepth;
    else
      extracting.erase(key);
  }
  return bit;
}

ArrayRef<Value> BitBlaster::lowerValueToBits(Value value) {
  auto *it = loweredValues.find(value);
  if (it != loweredValues.end())
    return it->second;

  auto width = hw::getBitWidth(value.getType());
  if (width <= 1)
    return insertBits(value, {value});

  auto *op = value.getDefiningOp();
  if (!op) {
    SmallVector<Value> results;
    OpBuilder builder(value.getContext());
    builder.setInsertionPointAfterValue(value);
    comb::extractBits(builder, value, results);
    return insertBits(value, std::move(results));
  }

  // The value depends on itself at word level: hand out placeholders for its
  // bits, replaced by the bits once the value is lowered.
  if (!inProgress.insert(value).second) {
    auto &bits = placeholderBits[value];
    if (bits.empty()) {
      OpBuilder builder(op);
      for (int64_t i = 0; i < width; ++i) {
        auto bit = mlir::UnrealizedConversionCastOp::create(
                       builder, value.getLoc(), builder.getI1Type(),
                       ValueRange{})
                       .getResult(0);
        placeholders.insert({bit, {value, static_cast<unsigned>(i)}});
        bits.push_back(bit);
      }
    }
    return placeholderBits[value];
  }

  ++depth;
  auto bits = TypeSwitch<Operation *, ArrayRef<Value>>(op)
      .Case<ChoiceOp>([&](ChoiceOp op) {
        auto createOp = [&](OpBuilder &builder, ValueRange operands) {
          return builder.createOrFold<ChoiceOp>(
              op.getLoc(), operands[0].getType(), operands);
        };
        return lowerOp(op, createOp);
      })
      .Case<BooleanLogicOpInterface>(
          [&](auto op) { return lowerBooleanLogicOperation(op); })
      .Case<comb::AndOp, comb::OrOp, comb::XorOp>(
          [&](auto op) { return lowerCombLogicOperations(op); })
      .Case<comb::MuxOp>([&](comb::MuxOp op) { return lowerCombMux(op); })
      .Default([&](auto op) {
        OpBuilder builder(value.getContext());
        builder.setInsertionPoint(op);
        SmallVector<Value> results;
        comb::extractBits(builder, value, results);

        return insertBits(value, std::move(results));
      });
  --depth;
  inProgress.erase(value);
  return bits;
}

LogicalResult BitBlaster::resolvePlaceholders() {
  if (placeholders.empty())
    return success();

  // The bit a placeholder stands for may itself be a placeholder (a bit that
  // is just another bit of a word on the cycle); follow such chains.
  DenseMap<Value, Value> resolved;
  for (auto &[placeholder, source] : placeholders) {
    Value bit = placeholder;
    SmallPtrSet<void *, 4> seen;
    while (placeholders.count(bit)) {
      if (auto it = resolved.find(bit); it != resolved.end()) {
        bit = it->second;
        break;
      }
      if (!seen.insert(bit.getAsOpaquePointer()).second)
        return mlir::emitError(source.first.getLoc(), "bit ")
               << source.second << " of this value is driven by itself";
      auto [value, index] = placeholders.lookup(bit);
      bit = loweredValues.find(value)->second[index];
    }
    resolved[placeholder] = bit;
  }

  for (auto &[value, bits] : loweredValues)
    for (auto &bit : bits)
      if (auto it = resolved.find(bit); it != resolved.end())
        bit = it->second;
  for (auto &[placeholder, bit] : resolved) {
    placeholder.replaceAllUsesWith(bit);
    placeholder.getDefiningOp()->erase();
  }
  return success();
}

LogicalResult BitBlaster::run() {
  // Topologically sort operations in graph regions so that walk visits them in
  // the topological order, which keeps the recursion shallow. With word-level
  // cycles the order is partial; the placeholders handle the rest.
  (void)topologicallySortGraphRegionBlocks(
      topOp, [](Value value, Operation *op) -> bool {
        // Otherthan target ops, all other ops are always ready.
        return !(shouldLowerOperation(op) ||
                 isa<comb::ExtractOp, comb::ReplicateOp, comb::ConcatOp,
                     comb::ReplicateOp>(op));
      });

  // Lower target operations
  topOp->walk([&](Operation *op) {
    if (shouldLowerOperation(op))
      (void)lowerValueToBits(op->getResult(0));
  });
  if (wiringCycle || failed(resolvePlaceholders()))
    return failure();

  // Replace operations with concatenated results if needed
  for (auto &[value, results] :
       llvm::make_early_inc_range(llvm::reverse(loweredValues))) {
    if (hw::getBitWidth(value.getType()) <= 1)
      continue;

    auto *op = value.getDefiningOp();
    if (!op)
      continue;

    if (value.use_empty()) {
      op->erase();
      continue;
    }

    // If a target operation still has an use (e.g. connected to output or
    // instance), replace the value with the concatenated result.
    if (shouldLowerOperation(op)) {
      OpBuilder builder(op);
      std::reverse(results.begin(), results.end());
      auto concat = comb::ConcatOp::create(builder, value.getLoc(), results);
      value.replaceAllUsesWith(concat);
      op->erase();
    }
  }

  // Wiring that read bits of the lowered words is left without users; with
  // word-level cycles it would keep referring to the words.
  if (!placeholders.empty()) {
    SmallVector<Operation *> worklist;
    topOp->walk([&](Operation *op) {
      if (isa<comb::ExtractOp, comb::ConcatOp, comb::ReplicateOp>(op))
        worklist.push_back(op);
    });
    DenseSet<Operation *> erased;
    while (!worklist.empty()) {
      auto *op = worklist.pop_back_val();
      if (erased.contains(op) || !op->use_empty())
        continue;
      erased.insert(op);
      for (auto operand : op->getOperands())
        if (auto *def = operand.getDefiningOp();
            def && isa<comb::ExtractOp, comb::ConcatOp, comb::ReplicateOp>(def))
          worklist.push_back(def);
      op->erase();
    }
  }

  return success();
}

Value BitBlaster::getBoolConstant(bool value, Block *block) {
  auto &blockConstants = constantsByBlock[block];
  if (!blockConstants[value]) {
    auto builder = OpBuilder::atBlockBegin(block);
    blockConstants[value] = hw::ConstantOp::create(
        builder, builder.getUnknownLoc(), builder.getI1Type(), value);
  }
  return blockConstants[value];
}

ArrayRef<Value>
BitBlaster::lowerBooleanLogicOperation(BooleanLogicOpInterface op) {
  auto createOp = [&](OpBuilder &builder, ValueRange operands) {
    return op.cloneWithSameInversion(builder, operands);
  };
  return lowerOp(op.getOperation(), createOp);
}

template <typename OpTy>
ArrayRef<Value> BitBlaster::lowerCombLogicOperations(OpTy op) {
  auto createOp = [&](OpBuilder &builder, ValueRange operands) {
    return builder.createOrFold<OpTy>(op.getLoc(), operands,
                                      op.getTwoStateAttr());
  };
  return lowerOp(op, createOp);
}

ArrayRef<Value> BitBlaster::lowerCombMux(comb::MuxOp op) {
  auto createOp = [&](OpBuilder &builder, ValueRange operands) {
    return builder.createOrFold<comb::MuxOp>(op.getLoc(), operands[0],
                                             operands[1], operands[2],
                                             op.getTwoStateAttr());
  };
  return lowerOp(op, createOp);
}

ArrayRef<Value> BitBlaster::lowerOp(
    Operation *op,
    llvm::function_ref<Value(OpBuilder &builder, ValueRange)> createOp) {
  auto value = op->getResult(0);
  OpBuilder builder(op);
  auto width = hw::getBitWidth(value.getType());
  assert(width > 1 && "expected multi-bit operation");

  auto known = computeKnownBits(value);
  APInt knownMask = known.Zero | known.One;

  // Update statistics
  numLoweredConstants += knownMask.popcount();
  numLoweredBits += width;
  ++numLoweredOps;

  SmallVector<Value> results;
  results.reserve(width);

  for (int64_t i = 0; i < width; ++i) {
    SmallVector<Value> operands;
    operands.reserve(op->getNumOperands());
    if (knownMask[i]) {
      // Use known constant value
      results.push_back(getBoolConstant(known.One[i], op->getBlock()));
      continue;
    }

    // Extract the i-th bit from each operand
    for (auto operand : op->getOperands())
      operands.push_back(extractBit(operand, i));

    // Create the single-bit operation
    auto result = createOp(builder, operands);
    results.push_back(result);

    // Add name hint if present
    if (auto name = op->getAttrOfType<StringAttr>("sv.namehint")) {
      auto newName = StringAttr::get(
          op->getContext(), name.getValue() + "[" + std::to_string(i) + "]");
      if (auto *loweredOp = result.getDefiningOp())
        loweredOp->setAttr("sv.namehint", newName);
    }
  }

  assert(results.size() == static_cast<size_t>(width));
  return insertBits(value, std::move(results));
}

//===----------------------------------------------------------------------===//
// Pass Implementation
//===----------------------------------------------------------------------===//

namespace {
struct LowerWordToBitsPass
    : public impl::LowerWordToBitsBase<LowerWordToBitsPass> {
  void runOnOperation() override;
};
} // namespace

void LowerWordToBitsPass::runOnOperation() {
  BitBlaster driver(getOperation());
  if (failed(driver.run()))
    return signalPassFailure();

  // Update statistics
  numLoweredBits += driver.numLoweredBits;
  numLoweredConstants += driver.numLoweredConstants;
  numLoweredOps += driver.numLoweredOps;
}
