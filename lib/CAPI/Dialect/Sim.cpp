//===- Sim.cpp - C interface for the Sim dialect --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt-c/Dialect/Sim.h"
#include "circt/Dialect/Sim/ExportDPIInterface.h"
#include "circt/Dialect/Sim/SimDialect.h"
#include "circt/Dialect/Sim/SimPasses.h"
#include "circt/Dialect/Sim/SimTypes.h"

#include "mlir/CAPI/IR.h"
#include "mlir/CAPI/Registration.h"
#include "mlir/CAPI/Support.h"
#include "mlir/CAPI/Utils.h"
#include "llvm/ADT/SmallVector.h"

#include "circt/Dialect/Sim/SimEnums.h.inc"

using namespace circt;
using namespace circt::sim;

//===----------------------------------------------------------------------===//
// Dialect API.
//===----------------------------------------------------------------------===//

MLIR_DEFINE_CAPI_DIALECT_REGISTRATION(Sim, sim, SimDialect)

void registerSimPasses() { circt::sim::registerPasses(); }

MlirLogicalResult mlirExportDPIInterface(MlirModule module,
                                         MlirStringCallback callback,
                                         void *userData) {
  mlir::detail::CallbackOstream output(callback, userData);
  return wrap(sim::exportDPIInterface(unwrap(module), output));
}

bool simTypeIsAFormatString(MlirType type) {
  return isa<FormatStringType>(unwrap(type));
}

MlirType simFormatStringTypeGet(MlirContext ctx) {
  return wrap(FormatStringType::get(unwrap(ctx)));
}

bool simTypeIsADynamicString(MlirType type) {
  return isa<DynamicStringType>(unwrap(type));
}

MlirType simDynamicStringTypeGet(MlirContext ctx) {
  return wrap(DynamicStringType::get(unwrap(ctx)));
}

bool simTypeIsAQueue(MlirType type) { return isa<QueueType>(unwrap(type)); }

MlirType simQueueTypeGet(MlirType elementType, unsigned bound) {
  return wrap(QueueType::get(unwrap(elementType), bound));
}

MlirType simQueueTypeGetElementType(MlirType type) {
  return wrap(cast<QueueType>(unwrap(type)).getElementType());
}

unsigned simQueueTypeGetBound(MlirType type) {
  return cast<QueueType>(unwrap(type)).getBound();
}

bool simTypeIsAAssocArray(MlirType type) {
  return isa<AssocArrayType>(unwrap(type));
}

MlirType simAssocArrayTypeGet(MlirType elementType, MlirType indexType) {
  return wrap(AssocArrayType::get(unwrap(elementType), unwrap(indexType)));
}

MlirType simAssocArrayTypeGetElementType(MlirType type) {
  return wrap(cast<AssocArrayType>(unwrap(type)).getElementType());
}

MlirType simAssocArrayTypeGetIndexType(MlirType type) {
  return wrap(cast<AssocArrayType>(unwrap(type)).getIndexType());
}

bool simTypeIsADPIFunction(MlirType type) {
  return isa<DPIFunctionType>(unwrap(type));
}

MlirType simDPIFunctionTypeGet(MlirContext ctx, intptr_t numArguments,
                               const SimDPIArgument *arguments) {
  llvm::SmallVector<DPIArgument> args;
  args.reserve(numArguments);
  for (intptr_t i = 0; i < numArguments; ++i)
    args.push_back(
        {mlir::StringAttr::get(unwrap(ctx), unwrap(arguments[i].name)),
         unwrap(arguments[i].type),
         static_cast<DPIDirection>(arguments[i].direction)});
  return wrap(DPIFunctionType::get(unwrap(ctx), args));
}

intptr_t simDPIFunctionTypeGetNumArguments(MlirType type) {
  return cast<DPIFunctionType>(unwrap(type)).getNumArguments();
}

SimDPIArgument simDPIFunctionTypeGetArgument(MlirType type, intptr_t index) {
  auto arg = cast<DPIFunctionType>(unwrap(type)).getArguments()[index];
  return {wrap(arg.name.getValue()), wrap(arg.type),
          static_cast<SimDPIDirection>(arg.dir)};
}

MlirType simDPIFunctionTypeGetFunctionType(MlirType type) {
  return wrap(cast<DPIFunctionType>(unwrap(type)).getFunctionType());
}

bool simTypeIsAOutputStream(MlirType type) {
  return isa<OutputStreamType>(unwrap(type));
}

MlirType simOutputStreamTypeGet(MlirContext ctx) {
  return wrap(OutputStreamType::get(unwrap(ctx)));
}
