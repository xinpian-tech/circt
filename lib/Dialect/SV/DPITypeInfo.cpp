//===- DPITypeInfo.cpp - DPI type mapping
//----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/SV/DPITypeInfo.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/SV/SVTypes.h"
#include "mlir/IR/SymbolTable.h"
#include "llvm/ADT/STLExtras.h"

using namespace circt;
using namespace mlir;
using sv::DPIIntegerContext;
using sv::DPITypeInfo;

StringRef DPITypeInfo::getIntegerKeyword() const {
  switch (kind) {
  case Kind::Bit:
    return "bit";
  case Kind::Logic:
    return "logic";
  case Kind::Byte:
    return "byte";
  case Kind::ShortInt:
    return "shortint";
  case Kind::Int:
    return "int";
  case Kind::LongInt:
    return "longint";
  default:
    return {};
  }
}

bool DPITypeInfo::isIntegerAtom() const {
  return kind == Kind::Byte || kind == Kind::ShortInt || kind == Kind::Int ||
         kind == Kind::LongInt;
}

bool DPITypeInfo::isValidReturn() const {
  return isIntegerAtom() ||
         ((kind == Kind::Bit || kind == Kind::Logic) && width == 1);
}

DPITypeInfo sv::getDPIIntegerTypeInfo(unsigned width,
                                      DPIIntegerContext context) {
  DPITypeInfo result{context == DPIIntegerContext::Typedef
                         ? DPITypeInfo::Kind::Logic
                         : DPITypeInfo::Kind::Bit};
  result.width = width;
  if (context != DPIIntegerContext::Import)
    return result;
  switch (width) {
  case 8:
    result.kind = DPITypeInfo::Kind::Byte;
    break;
  case 16:
    result.kind = DPITypeInfo::Kind::ShortInt;
    break;
  case 32:
    result.kind = DPITypeInfo::Kind::Int;
    break;
  case 64:
    result.kind = DPITypeInfo::Kind::LongInt;
    break;
  }
  result.isSigned = result.isIntegerAtom();
  return result;
}

DPITypeInfo sv::getDPIEnumTypeInfo(unsigned width) {
  DPITypeInfo result{DPITypeInfo::Kind::Enum};
  result.width = width;
  result.isSigned = width == 32;
  return result;
}

namespace {

class Resolver {
public:
  explicit Resolver(Operation *symbolTable) : symbolTable(symbolTable) {}

  FailureOr<DPITypeInfo> resolve(Type type, DPIIntegerContext context) {
    if (auto alias = dyn_cast<hw::TypeAliasType>(type)) {
      auto decl = dyn_cast_or_null<hw::TypedeclOp>(
          symbols.lookupSymbolIn(symbolTable, alias.getRef()));
      if (!decl || decl.getType() != alias.getInnerType())
        return failure();
      // The importer names the typedef. Its definition is printed in the
      // ordinary four-state type context, not the DPI integer atom context.
      return resolve(decl.getType(), DPIIntegerContext::Typedef);
    }
    if (auto integer = dyn_cast<IntegerType>(type)) {
      if (!integer.getWidth())
        return failure();
      return sv::getDPIIntegerTypeInfo(integer.getWidth(), context);
    }
    if (auto array = dyn_cast<hw::ArrayType>(type))
      return resolveArray(array.getElementType(), array.getSizeAttr(), true,
                          context);
    if (auto array = dyn_cast<hw::UnpackedArrayType>(type))
      return resolveArray(array.getElementType(), array.getSizeAttr(), false,
                          context);
    if (auto array = dyn_cast<sv::UnpackedOpenArrayType>(type))
      return resolveArray(array.getElementType(), {}, false, context);
    if (auto structure = dyn_cast<hw::StructType>(type)) {
      DPITypeInfo result{DPITypeInfo::Kind::Struct};
      result.packed = true;
      for (auto field : structure.getElements()) {
        auto member = resolve(field.type, memberContext(context));
        if (failed(member) || !isPacked(*member))
          return failure();
        member->name = field.name;
        result.elements.push_back(std::move(*member));
      }
      if (result.elements.empty())
        return failure();
      return result;
    }
    if (auto unionType = dyn_cast<hw::UnionType>(type)) {
      DPITypeInfo result{DPITypeInfo::Kind::Union};
      result.packed = true;
      for (auto field : unionType.getElements()) {
        auto member = resolve(field.type, memberContext(context));
        if (failed(member) || !isPacked(*member))
          return failure();
        member->name = field.name;
        member->offset = field.offset;
        result.elements.push_back(std::move(*member));
      }
      if (result.elements.empty())
        return failure();
      return result;
    }
    if (auto enumeration = dyn_cast<hw::EnumType>(type)) {
      auto width = enumeration.getBitWidth();
      if (!width || !*width)
        return failure();
      auto result = sv::getDPIEnumTypeInfo(*width);
      for (auto name : enumeration.getFields().getAsRange<StringAttr>()) {
        auto member =
            sv::getDPIIntegerTypeInfo(*width, DPIIntegerContext::Packed);
        member.name = name;
        result.elements.push_back(std::move(member));
      }
      return result;
    }
    return failure();
  }

private:
  static DPIIntegerContext memberContext(DPIIntegerContext context) {
    return context == DPIIntegerContext::Typedef ? context
                                                 : DPIIntegerContext::Import;
  }

  static bool isPacked(const DPITypeInfo &info) {
    return info.kind != DPITypeInfo::Kind::Array ||
           (info.packed && isPacked(info.elements.front()));
  }

  FailureOr<DPITypeInfo> resolveArray(Type elementType, Attribute sizeAttr,
                                      bool packed, DPIIntegerContext context) {
    DPITypeInfo result{DPITypeInfo::Kind::Array};
    result.packed = packed;
    if (sizeAttr) {
      auto size = dyn_cast<IntegerAttr>(sizeAttr);
      if (!size || size.getValue().isNegative() || size.getValue().isZero() ||
          size.getValue().getActiveBits() > 64)
        return failure();
      result.size = size.getValue().getZExtValue();
    }
    auto element =
        resolve(elementType, packed && context != DPIIntegerContext::Typedef
                                 ? DPIIntegerContext::Packed
                                 : context);
    if (failed(element) || (packed && !isPacked(*element)))
      return failure();
    result.elements.push_back(std::move(*element));
    return result;
  }

  Operation *symbolTable;
  SymbolTableCollection symbols;
};

} // namespace

FailureOr<DPITypeInfo> sv::resolveDPIType(Type type, Operation *symbolTable) {
  return Resolver(symbolTable).resolve(type, DPIIntegerContext::Import);
}
