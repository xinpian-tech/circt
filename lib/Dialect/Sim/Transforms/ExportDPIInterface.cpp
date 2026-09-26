//===- ExportDPIInterface.cpp - Export DPI function ABIs ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Sim/ExportDPIInterface.h"
#include "circt/Dialect/HW/HWTypes.h"
#include "circt/Dialect/Moore/MooreTypes.h"
#include "circt/Dialect/SV/SVTypes.h"
#include "circt/Dialect/Sim/SimOps.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>

using namespace mlir;
using namespace circt;

namespace {

bool isIntegralType(Type type) {
  if (auto alias = dyn_cast<hw::TypeAliasType>(type))
    return isIntegralType(alias.getInnerType());
  if (isa<IntegerType, hw::EnumType>(type))
    return true;
  if (auto array = dyn_cast<hw::ArrayType>(type))
    return isIntegralType(array.getElementType());
  if (auto structure = dyn_cast<hw::StructType>(type))
    return llvm::all_of(structure.getElements(),
                        [](auto field) { return isIntegralType(field.type); });
  if (auto unionType = dyn_cast<hw::UnionType>(type))
    return llvm::all_of(unionType.getElements(),
                        [](auto field) { return isIntegralType(field.type); });
  if (auto packed = dyn_cast<moore::PackedType>(type))
    return packed.getBitSize().value_or(0) > 0;
  return false;
}

bool isScalarIntegerWidth(unsigned width) {
  return width == 1 || width == 8 || width == 16 || width == 32 || width == 64;
}

FailureOr<llvm::json::Object> exportType(Type type, bool isReturn = false);

FailureOr<llvm::json::Object>
exportArray(Type elementType, std::optional<uint64_t> size, bool packed) {
  if ((size && *size == 0) || (packed && !isIntegralType(elementType)))
    return failure();
  auto element = exportType(elementType);
  if (failed(element))
    return failure();
  llvm::json::Object array{
      {"type", "array"}, {"packed", packed}, {"element", std::move(*element)}};
  array["size"] = size ? llvm::json::Value(*size) : llvm::json::Value(nullptr);
  return array;
}

template <typename Fields>
FailureOr<llvm::json::Object> exportFields(Fields fields, StringRef kind,
                                           bool packed) {
  llvm::json::Array members;
  for (auto field : fields) {
    if (packed && !isIntegralType(field.type))
      return failure();
    auto member = exportType(field.type);
    if (failed(member))
      return failure();
    (*member)["name"] = field.name.getValue().str();
    members.push_back(std::move(*member));
  }
  if (members.empty())
    return failure();
  return llvm::json::Object{{"type", kind.str()},
                            {"packed", packed},
                            {"members", std::move(members)}};
}

// IEEE 1800-2023 35.5.5/35.5.6 and Annex H distinguish argument types from
// the smaller set of function result types. Do not infer ABI from opaque
// pointers or silently serialize arbitrary IR types as if they were DPI types.
FailureOr<llvm::json::Object> exportType(Type type, bool isReturn) {
  if (auto alias = dyn_cast<hw::TypeAliasType>(type))
    return exportType(alias.getInnerType(), isReturn);
  if (auto integer = dyn_cast<IntegerType>(type)) {
    if (!integer.getWidth() ||
        (isReturn && !isScalarIntegerWidth(integer.getWidth())))
      return failure();
    return llvm::json::Object{{"width", integer.getWidth()},
                              {"signed", integer.isSigned()}};
  }
  if (auto integer = dyn_cast<moore::IntType>(type)) {
    bool fourState = integer.getDomain() == moore::Domain::FourValued;
    if (!integer.getWidth() ||
        (isReturn && (fourState ? integer.getWidth() != 1
                                : !isScalarIntegerWidth(integer.getWidth()))))
      return failure();
    llvm::json::Object result{{"width", integer.getWidth()}, {"signed", false}};
    if (fourState)
      result["type"] = "logic";
    return result;
  }
  if (isa<Float32Type>(type))
    return llvm::json::Object{{"type", "shortreal"}};
  if (isa<Float64Type>(type))
    return llvm::json::Object{{"type", "real"}};
  if (auto real = dyn_cast<moore::RealType>(type))
    return llvm::json::Object{{"type", real.getWidth() == moore::RealWidth::f32
                                           ? "shortreal"
                                           : "real"}};
  if (isa<hw::StringType, sim::DynamicStringType, moore::StringType>(type))
    return llvm::json::Object{{"type", "string"}};
  if (isa<moore::ChandleType>(type))
    return llvm::json::Object{{"type", "chandle"}};
  if (isReturn)
    return failure();
  if (isa<moore::TimeType>(type))
    return llvm::json::Object{{"type", "time"}};
  if (auto array = dyn_cast<hw::ArrayType>(type))
    return exportArray(array.getElementType(), array.getNumElements(), true);
  if (auto array = dyn_cast<hw::UnpackedArrayType>(type))
    return exportArray(array.getElementType(), array.getNumElements(), false);
  if (auto array = dyn_cast<sv::UnpackedOpenArrayType>(type))
    return exportArray(array.getElementType(), std::nullopt, false);
  if (auto array = dyn_cast<moore::ArrayType>(type))
    return exportArray(array.getElementType(), array.getSize(), true);
  if (auto array = dyn_cast<moore::UnpackedArrayType>(type))
    return exportArray(array.getElementType(), array.getSize(), false);
  if (auto array = dyn_cast<moore::OpenArrayType>(type))
    return exportArray(array.getElementType(), std::nullopt, true);
  if (auto array = dyn_cast<moore::OpenUnpackedArrayType>(type))
    return exportArray(array.getElementType(), std::nullopt, false);
  if (auto structure = dyn_cast<hw::StructType>(type))
    return exportFields(structure.getElements(), "struct", true);
  if (auto unionType = dyn_cast<hw::UnionType>(type)) {
    auto result = exportFields(unionType.getElements(), "union", true);
    if (succeeded(result))
      for (auto [member, field] :
           llvm::zip(*result->getArray("members"), unionType.getElements()))
        (*member.getAsObject())["offset"] = field.offset;
    return result;
  }
  if (auto structure = dyn_cast<moore::StructType>(type))
    return exportFields(structure.getMembers(), "struct", true);
  if (auto structure = dyn_cast<moore::UnpackedStructType>(type))
    return exportFields(structure.getMembers(), "struct", false);
  if (auto unionType = dyn_cast<moore::UnionType>(type))
    return exportFields(unionType.getMembers(), "union", true);
  if (auto enumeration = dyn_cast<hw::EnumType>(type)) {
    auto width = enumeration.getBitWidth();
    if (!width || *width == 0)
      return failure();
    llvm::json::Array members;
    for (auto field : enumeration.getFields().getAsRange<StringAttr>())
      members.push_back(field.getValue().str());
    return llvm::json::Object{{"type", "enum"},
                              {"width", *width},
                              {"signed", false},
                              {"members", std::move(members)}};
  }
  return failure();
}

} // namespace

LogicalResult sim::exportDPIInterface(mlir::ModuleOp module,
                                      llvm::raw_ostream &output) {
  llvm::json::Array functions;
  for (auto func : module.getOps<sim::DPIFuncOp>()) {
    llvm::json::Array args;
    auto dpiArgs = func.getDpiFunctionType().getArguments();
    for (const auto &arg : dpiArgs) {
      if (arg.dir == sim::DPIDirection::Ref) {
        func.emitError("IEEE 1800 DPI does not allow ref arguments");
        return failure();
      }
      auto argument =
          exportType(arg.type, arg.dir == sim::DPIDirection::Return);
      if (failed(argument)) {
        func.emitError() << "unsupported DPI schema argument " << arg.name
                         << " of type " << arg.type << " with direction "
                         << sim::stringifyDPIDirectionKeyword(arg.dir);
        return failure();
      }
      (*argument)["name"] = arg.name.getValue().str();
      (*argument)["direction"] =
          sim::stringifyDPIDirectionKeyword(arg.dir).str();
      args.push_back(std::move(*argument));
    }
    functions.push_back(llvm::json::Object{
        {"function", func.getVerilogName().value_or(func.getSymName()).str()},
        {"arguments", std::move(args)}});
  }
  if (functions.empty()) {
    module.emitError("no sim.func.dpi operations to export");
    return failure();
  }

  llvm::json::Object schema{{"dpi_functions", std::move(functions)}};
  output << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(schema)));
  return success();
}
