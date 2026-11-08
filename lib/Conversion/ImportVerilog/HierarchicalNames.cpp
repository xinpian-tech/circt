//===- HierarchicalNames.cpp - Cross-module references --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Hierarchical names that refer to a variable or net of another instance
// become `moore.xmr` ops: the path from the referencing module up to the
// common ancestor and down to the module of the target, by instance name, and
// the name of the target in that module. Names are formed as the conversion of
// modules forms them (generate block prefixes, instance arrays, flattened
// interface instances), so `moore-resolve-xmr` finds the instances and the
// target by name.
//
//===----------------------------------------------------------------------===//

#include "ImportVerilogInternals.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace circt;
using namespace ImportVerilog;
using slang::ast::DefinitionKind;
using slang::ast::SymbolKind;

/// The prefix that generate blocks between `symbol` and the instance body
/// containing it add to its name (see `ModuleVisitor`), or failure if
/// `symbol` is in a scope that the module conversion does not name this way.
static FailureOr<std::string> getBlockNamePrefix(const slang::ast::Symbol &symbol) {
  SmallVector<std::string> pieces;
  const slang::ast::Scope *scope = symbol.getParentScope();
  while (scope) {
    auto &scopeSym = scope->asSymbol();
    if (scopeSym.kind == SymbolKind::InstanceBody)
      break;
    if (scopeSym.kind == SymbolKind::InstanceArray) {
      scope = scopeSym.getParentScope();
      continue;
    }
    auto *gen = scopeSym.as_if<slang::ast::GenerateBlockSymbol>();
    if (!gen)
      return failure();
    auto &parent = gen->getParentScope()->asSymbol();
    if (auto *array = parent.as_if<slang::ast::GenerateBlockArraySymbol>()) {
      std::string piece = std::string(array->getExternalName()) + "_";
      if (gen->arrayIndex)
        piece += gen->arrayIndex->toString();
      else
        piece += std::to_string(gen->constructIndex);
      piece += ".";
      if (!gen->name.empty())
        piece += std::string(gen->getExternalName()) + ".";
      pieces.push_back(piece);
      scope = array->getParentScope();
      continue;
    }
    pieces.push_back(std::string(gen->getExternalName()) + ".");
    scope = gen->getParentScope();
  }
  std::string prefix;
  for (auto &piece : llvm::reverse(pieces))
    prefix += piece;
  return prefix;
}

/// The name of the `moore.instance` op of `inst` (see `ModuleVisitor`).
static FailureOr<std::string>
getInstanceName(const slang::ast::InstanceSymbol &inst) {
  auto prefix = getBlockNamePrefix(inst);
  if (failed(prefix))
    return failure();
  SmallString<64> name(*prefix);
  if (inst.arrayPath.empty()) {
    name += inst.name;
  } else {
    name += inst.getArrayName();
    slang::SmallVector<slang::ConstantRange, 4> dims;
    inst.getArrayDimensions(dims);
    for (auto [dim, index] : llvm::zip(dims, inst.arrayPath)) {
      name += '_';
      Twine(dim.lower() + int32_t(index)).toVector(name);
    }
  }
  return std::string(name);
}

/// The instance names from the top instance down to `body`, and the name of
/// the top instance.
static LogicalResult
getInstancePath(const slang::ast::InstanceBodySymbol *body,
                SmallVectorImpl<std::string> &path, std::string &top) {
  while (body && body->parentInstance) {
    auto *parent =
        body->parentInstance->getParentScope()->getContainingInstance();
    if (!parent) {
      top = std::string(body->parentInstance->name);
      std::reverse(path.begin(), path.end());
      return success();
    }
    auto name = getInstanceName(*body->parentInstance);
    if (failed(name))
      return failure();
    path.push_back(*name);
    body = parent;
  }
  return failure();
}

FailureOr<XmrSpec>
Context::getXmrSpec(const slang::ast::InstanceBodySymbol &from,
                    const slang::ast::ValueSymbol &symbol, Location loc) {
  auto unsupported = [&](const Twine &why) {
    mlir::emitError(loc) << "unsupported hierarchical reference to `"
                         << symbol.name << "`: " << why;
    return failure();
  };

  auto *body = symbol.getParentScope()->getContainingInstance();
  if (!body)
    return unsupported("not declared in a module instance");
  auto prefix = getBlockNamePrefix(symbol);
  if (failed(prefix))
    return unsupported("declared in a scope other than a generate block");
  XmrSpec spec;
  spec.target = *prefix + std::string(symbol.name);

  // Interface instances are flattened into the instantiating module, their
  // members are named `<instance>_<member>`.
  if (body->getDefinition().definitionKind == DefinitionKind::Interface) {
    auto &ifaceInst = *body->parentInstance;
    auto ifacePrefix = getBlockNamePrefix(ifaceInst);
    if (failed(ifacePrefix))
      return unsupported("interface instance in an unsupported scope");
    spec.target =
        *ifacePrefix + std::string(ifaceInst.name) + "_" + spec.target;
    body = ifaceInst.getParentScope()->getContainingInstance();
    if (!body)
      return unsupported("interface instance outside of a module");
  }

  SmallVector<std::string> fromPath, toPath;
  std::string fromTop, toTop;
  if (failed(getInstancePath(&from, fromPath, fromTop)) ||
      failed(getInstancePath(body, toPath, toTop)))
    return unsupported("instance in an unsupported scope");
  if (fromTop != toTop)
    return unsupported("refers to another top-level instance");

  unsigned common = 0;
  while (common < fromPath.size() && common < toPath.size() &&
         fromPath[common] == toPath[common])
    ++common;
  spec.levels = fromPath.size() - common;
  spec.path.assign(toPath.begin() + common, toPath.end());
  return spec;
}

Value Context::materializeXmr(const slang::ast::ValueSymbol &symbol,
                              Location loc) {
  if (!currentBody) {
    mlir::emitError(loc) << "hierarchical reference to `" << symbol.name
                         << "` outside of a module";
    return {};
  }
  auto spec = getXmrSpec(*currentBody, symbol, loc);
  if (failed(spec))
    return {};
  auto type = convertType(symbol.getType());
  if (!type)
    return {};
  auto unpacked = dyn_cast<moore::UnpackedType>(type);
  if (!unpacked) {
    mlir::emitError(loc) << "unsupported type of hierarchical reference to `"
                         << symbol.name << "`: " << type;
    return {};
  }
  SmallVector<Attribute> path;
  for (auto &name : spec->path)
    path.push_back(builder.getStringAttr(name));
  return moore::XMROp::create(builder, loc, moore::RefType::get(unpacked),
                              spec->levels, builder.getArrayAttr(path),
                              builder.getStringAttr(spec->target));
}

namespace {
/// Visit all instance bodies, not only the canonical ones that are converted,
/// and compare the cross-module references of each body with those of its
/// canonical body.
struct HierarchicalNameChecker
    : public slang::ast::ASTVisitor<HierarchicalNameChecker,
                                    slang::ast::VisitFlags::AllGood> {
  HierarchicalNameChecker(Context &context) : context(context) {}

  void handle(const slang::ast::InstanceSymbol &inst) {
    // Port connections are expressions of the instantiating module.
    for (auto *conn : inst.getPortConnections())
      if (auto *expr = conn->getExpression())
        expr->visit(*this);
    // Interface instances are flattened into the instantiating module.
    if (inst.body.getDefinition().definitionKind == DefinitionKind::Interface) {
      for (auto &member : inst.body.members())
        member.visit(*this);
      return;
    }
    llvm::SaveAndRestore guard(current, &inst.body);
    for (auto &member : inst.body.members())
      member.visit(*this);
  }

  void handle(const slang::ast::HierarchicalValueExpression &expr) {
    visitDefault(expr);
    if (!current || expr.ref.isViaIfacePort() ||
        isCompileTimeConstant(expr.symbol.kind))
      return;
    auto *symBody = expr.symbol.getParentScope()->getContainingInstance();
    if (symBody == current)
      return;
    auto loc = context.convertLocation(expr.sourceRange);
    // Errors in forming the reference are reported by the conversion.
    mlir::ScopedDiagnosticHandler silence(context.getContext(),
                                          [](Diagnostic &) { return success(); });
    auto spec = context.getXmrSpec(*current, expr.symbol, loc);
    if (failed(spec))
      return;
    auto *canonical = current->parentInstance
                          ? getCanonicalBody(*current->parentInstance)
                          : current;
    auto key = std::make_pair(canonical, expr.sourceRange.start());
    auto [it, inserted] = seen.try_emplace(key, *spec, current);
    if (inserted || it->second.first == *spec)
      return;
    conflicts.push_back({&expr, it->second.second, current});
  }

  Context &context;
  const slang::ast::InstanceBodySymbol *current = nullptr;
  std::map<std::pair<const slang::ast::InstanceBodySymbol *,
                     slang::SourceLocation>,
           std::pair<XmrSpec, const slang::ast::InstanceBodySymbol *>>
      seen;
  struct Conflict {
    const slang::ast::HierarchicalValueExpression *expr;
    const slang::ast::InstanceBodySymbol *first, *second;
  };
  SmallVector<Conflict> conflicts;
};
} // namespace

static std::string getBodyPath(const slang::ast::InstanceBodySymbol &body) {
  return body.parentInstance ? body.parentInstance->getHierarchicalPath()
                             : std::string();
}

LogicalResult Context::checkHierarchicalNames() {
  HierarchicalNameChecker checker(*this);
  for (auto *inst : compilation.getRoot().topInstances)
    inst->visit(checker);
  for (auto &conflict : checker.conflicts)
    mlir::emitError(convertLocation(conflict.expr->sourceRange))
        << "hierarchical reference to `" << conflict.expr->symbol.name
        << "` resolves to different targets in instances `"
        << getBodyPath(*conflict.first) << "` and `"
        << getBodyPath(*conflict.second)
        << "` of the same module; not supported yet";
  return failure(!checker.conflicts.empty());
}
