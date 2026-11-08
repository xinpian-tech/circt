//===- ResolveXMR.cpp - Replace cross-module references by ports ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Replace `moore.xmr` ops by references passed through ports. For a reference
// `up N path [p1, ..., pk] target t` in module M:
//
// - `up(M, N)`: for N > 0, M gets an input port; every instance of M passes
//   `up(Parent, N - 1)` of the module it is in. For N = 0, the value is
//   `down(M, [p1, ..., pk])`.
// - `down(X, [p1, ...])`: the result of instance p1 of X for an output port
//   added to its module Y, which outputs `down(Y, [p2, ...])`; at the end of
//   the path, the variable or net named t.
//
// Ports are memoized per module and path, so references to the same target
// share them. Instance ops are rebuilt to add operands and results; values
// are used right after they are computed, so they stay valid (results of
// rebuilt instances are replaced with the results of the new ones).
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/HW/HWTypes.h"
#include "circt/Dialect/Moore/MooreOps.h"
#include "circt/Dialect/Moore/MoorePasses.h"
#include "llvm/ADT/StringSet.h"

namespace circt {
namespace moore {
#define GEN_PASS_DEF_RESOLVEXMR
#include "circt/Dialect/Moore/MoorePasses.h.inc"
} // namespace moore
} // namespace circt

using namespace circt;
using namespace moore;
using namespace mlir;

namespace {
struct Resolver {
  Resolver(ModuleOp root) : root(root), symbolTable(root) {}

  /// The value in `module` that refers to the target `levels` instances up
  /// and then down `path`.
  FailureOr<Value> up(SVModuleOp module, unsigned levels,
                      ArrayRef<StringAttr> path, StringAttr target,
                      RefType type, Operation *xmr);
  /// The value in `module` that refers to the target down `path`.
  FailureOr<Value> down(SVModuleOp module, ArrayRef<StringAttr> path,
                        StringAttr target, RefType type, Operation *xmr);
  /// The index of the output port of `module` that exports the target down
  /// `path`.
  FailureOr<unsigned> output(SVModuleOp module, ArrayRef<StringAttr> path,
                             StringAttr target, RefType type, Operation *xmr);

  /// All instances of `module`.
  SmallVector<InstanceOp> instancesOf(SVModuleOp module);
  /// Rebuild `inst` with an additional input or output.
  InstanceOp rebuild(InstanceOp inst, Value input, StringAttr inputName,
                     Type outputType, StringAttr outputName);
  /// A port name of `module` based on `hint` that is not used yet.
  StringAttr uniquePortName(SVModuleOp module, const Twine &hint);

  ModuleOp root;
  SymbolTable symbolTable;
  /// Input ports added for `up` (module, key) and output ports added for
  /// `output` (module, key).
  DenseMap<std::pair<Operation *, StringAttr>, Value> inputs;
  DenseMap<std::pair<Operation *, StringAttr>, unsigned> outputs;
};
} // namespace

static std::string pathKey(unsigned levels, ArrayRef<StringAttr> path,
                           StringAttr target) {
  std::string key = std::to_string(levels);
  for (auto name : path)
    key += "/" + name.getValue().str();
  key += ":" + target.getValue().str();
  return key;
}

static std::string pathName(ArrayRef<StringAttr> path, StringAttr target) {
  std::string name;
  for (auto elem : path)
    name += elem.getValue().str() + ".";
  name += target.getValue().str();
  return name;
}

SmallVector<InstanceOp> Resolver::instancesOf(SVModuleOp module) {
  SmallVector<InstanceOp> result;
  for (auto other : root.getOps<SVModuleOp>())
    other.walk([&](InstanceOp inst) {
      if (inst.getModuleNameAttr().getAttr() == module.getSymNameAttr())
        result.push_back(inst);
    });
  return result;
}

StringAttr Resolver::uniquePortName(SVModuleOp module, const Twine &hint) {
  llvm::StringSet<> used;
  for (auto port : module.getModuleType().getPorts())
    used.insert(port.name.getValue());
  std::string name = hint.str();
  for (unsigned i = 0; used.contains(name); ++i)
    name = (hint + "_" + Twine(i)).str();
  return StringAttr::get(module.getContext(), name);
}

InstanceOp Resolver::rebuild(InstanceOp inst, Value input,
                             StringAttr inputName, Type outputType,
                             StringAttr outputName) {
  OpBuilder builder(inst);
  SmallVector<Value> operands(inst.getInputs());
  SmallVector<Attribute> inputNames(inst.getInputNames().getValue());
  SmallVector<Type> resultTypes(inst->getResultTypes());
  SmallVector<Attribute> outputNames(inst.getOutputNames().getValue());
  if (input) {
    operands.push_back(input);
    inputNames.push_back(inputName);
  }
  if (outputType) {
    resultTypes.push_back(outputType);
    outputNames.push_back(outputName);
  }
  auto newInst = InstanceOp::create(
      builder, inst.getLoc(), resultTypes, inst.getInstanceNameAttr(),
      inst.getModuleNameAttr(), operands, builder.getArrayAttr(inputNames),
      builder.getArrayAttr(outputNames));
  for (auto attr : inst->getDiscardableAttrs())
    newInst->setAttr(attr.getName(), attr.getValue());
  inst.replaceAllUsesWith(
      newInst->getResults().take_front(inst->getNumResults()));
  inst.erase();
  return newInst;
}

FailureOr<Value> Resolver::up(SVModuleOp module, unsigned levels,
                              ArrayRef<StringAttr> path, StringAttr target,
                              RefType type, Operation *xmr) {
  if (levels == 0)
    return down(module, path, target, type, xmr);

  auto key = std::make_pair(
      module.getOperation(),
      StringAttr::get(module.getContext(), pathKey(levels, path, target)));
  if (auto value = inputs.lookup(key))
    return value;

  auto insts = instancesOf(module);
  if (insts.empty())
    return xmr->emitError("hierarchical reference goes above module `")
           << module.getSymName()
           << "`, which is not instantiated (it resolves to different "
              "targets in different instances, which is not supported yet)";

  // Add the input port, after the other inputs. A target in the common
  // ancestor itself is named after the ancestor (its instance name, or its
  // module name at the top).
  std::string hint = pathName(path, target);
  if (path.empty()) {
    SVModuleOp ancestor = module;
    InstanceOp ancestorInst;
    for (unsigned i = 0; i < levels && ancestor; ++i) {
      auto ancestorInsts = instancesOf(ancestor);
      if (ancestorInsts.empty())
        break;
      ancestorInst = ancestorInsts.front();
      ancestor = ancestorInst->getParentOfType<SVModuleOp>();
    }
    if (ancestor) {
      auto ancestorInsts = instancesOf(ancestor);
      hint = (ancestorInsts.empty()
                  ? ancestor.getSymName()
                  : ancestorInsts.front().getInstanceName())
                 .str() +
             "." + hint;
    }
  }
  auto name = uniquePortName(module, hint);
  SmallVector<hw::ModulePort> ports(module.getModuleType().getPorts());
  unsigned pos = 0;
  for (auto [i, port] : llvm::enumerate(ports))
    if (port.dir != hw::ModulePort::Output)
      pos = i + 1;
  ports.insert(ports.begin() + pos, {name, type, hw::ModulePort::Input});
  module.setModuleType(hw::ModuleType::get(module.getContext(), ports));
  auto arg = module.getBody()->addArgument(type, xmr->getLoc());
  inputs[key] = arg;

  // Pass the value of the parent into each instance. Instances may be rebuilt
  // while the parent's value is computed, so look them up again each time.
  auto numInputs = module.getModuleType().getNumInputs();
  while (true) {
    InstanceOp inst;
    for (auto candidate : instancesOf(module))
      if (candidate.getInputs().size() < numInputs) {
        inst = candidate;
        break;
      }
    if (!inst)
      break;
    auto parent = inst->getParentOfType<SVModuleOp>();
    auto value = up(parent, levels - 1, path, target, type, xmr);
    if (failed(value))
      return failure();
    // Look the instance up again: computing the value may have rebuilt it.
    InstanceOp current;
    for (auto candidate : instancesOf(module))
      if (candidate.getInputs().size() < numInputs &&
          candidate->getParentOp() == parent.getOperation() &&
          candidate.getInstanceNameAttr() == inst.getInstanceNameAttr())
        current = candidate;
    assert(current && "instance lost while resolving");
    rebuild(current, *value, name, {}, {});
  }
  return Value(arg);
}

FailureOr<Value> Resolver::down(SVModuleOp module, ArrayRef<StringAttr> path,
                                StringAttr target, RefType type,
                                Operation *xmr) {
  // The target itself.
  if (path.empty()) {
    Value found;
    for (auto &op : *module.getBody()) {
      StringAttr name;
      if (auto var = dyn_cast<VariableOp>(op))
        name = var.getNameAttr();
      else if (auto net = dyn_cast<NetOp>(op))
        name = net.getNameAttr();
      if (name != target)
        continue;
      if (found)
        return xmr->emitError("hierarchical reference target `")
               << target.getValue() << "` is not unique in module `"
               << module.getSymName() << "`";
      found = op.getResult(0);
    }
    if (!found)
      return xmr->emitError("hierarchical reference target `")
             << target.getValue() << "` not found in module `"
             << module.getSymName()
             << "` (it may resolve to different targets in different "
                "instances, which is not supported yet)";
    if (found.getType() != type)
      return xmr->emitError("hierarchical reference has type ")
             << type << ", but its target `" << target.getValue() << "` in `"
             << module.getSymName() << "` has type " << found.getType();
    return found;
  }

  // An instance of this module on the path.
  auto findInstance = [&]() -> InstanceOp {
    InstanceOp found;
    module.walk([&](InstanceOp inst) {
      if (inst.getInstanceNameAttr() == path.front())
        found = inst;
    });
    return found;
  };
  auto inst = findInstance();
  if (!inst)
    return xmr->emitError("hierarchical reference path: no instance `")
           << path.front().getValue() << "` in module `"
           << module.getSymName()
           << "` (it may resolve to different targets in different "
              "instances, which is not supported yet)";
  auto child = symbolTable.lookup<SVModuleOp>(inst.getModuleName());
  if (!child)
    return xmr->emitError("hierarchical reference path: instance `")
           << path.front().getValue() << "` is not a module instance";
  auto idx = output(child, path.drop_front(), target, type, xmr);
  if (failed(idx))
    return failure();
  inst = findInstance();
  return inst->getResult(*idx);
}

FailureOr<unsigned> Resolver::output(SVModuleOp module,
                                     ArrayRef<StringAttr> path,
                                     StringAttr target, RefType type,
                                     Operation *xmr) {
  auto key = std::make_pair(
      module.getOperation(),
      StringAttr::get(module.getContext(), pathKey(0, path, target)));
  if (auto it = outputs.find(key); it != outputs.end())
    return it->second;

  auto value = down(module, path, target, type, xmr);
  if (failed(value))
    return failure();

  // Add the output port and use the value right away.
  auto name = uniquePortName(module, pathName(path, target));
  auto outputOp = module.getOutputOp();
  outputOp->insertOperands(outputOp->getNumOperands(), *value);
  SmallVector<hw::ModulePort> ports(module.getModuleType().getPorts());
  ports.push_back({name, type, hw::ModulePort::Output});
  module.setModuleType(hw::ModuleType::get(module.getContext(), ports));
  unsigned idx = module.getModuleType().getNumOutputs() - 1;
  outputs[key] = idx;

  for (auto inst : instancesOf(module))
    rebuild(inst, {}, {}, type, name);
  return idx;
}

namespace {
struct ResolveXMRPass
    : public circt::moore::impl::ResolveXMRBase<ResolveXMRPass> {
  void runOnOperation() override;
};
} // namespace

void ResolveXMRPass::runOnOperation() {
  SmallVector<XMROp> xmrs;
  getOperation().walk([&](XMROp op) { xmrs.push_back(op); });
  if (xmrs.empty())
    return markAllAnalysesPreserved();

  Resolver resolver(getOperation());
  for (auto xmr : xmrs) {
    auto module = xmr->getParentOfType<SVModuleOp>();
    if (!module) {
      xmr.emitError("hierarchical reference outside of a module");
      return signalPassFailure();
    }
    SmallVector<StringAttr> path;
    for (auto elem : xmr.getPath())
      path.push_back(cast<StringAttr>(elem));
    auto value = resolver.up(module, xmr.getLevels(), path, xmr.getTargetAttr(),
                             xmr.getType(), xmr);
    if (failed(value))
      return signalPassFailure();
    xmr.replaceAllUsesWith(*value);
    xmr.erase();
  }
}
