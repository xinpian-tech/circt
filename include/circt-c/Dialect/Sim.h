//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_C_DIALECT_SIM_H
#define CIRCT_C_DIALECT_SIM_H

#include "mlir-c/IR.h"

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Dialect API.
//===----------------------------------------------------------------------===//

MLIR_DECLARE_CAPI_DIALECT_REGISTRATION(Sim, sim);
MLIR_CAPI_EXPORTED void registerSimPasses(void);

/// Export imported DPI function ABIs as JSON through the supplied callback.
MLIR_CAPI_EXPORTED MlirLogicalResult mlirExportDPIInterface(
    MlirModule module, MlirStringCallback callback, void *userData);

//===----------------------------------------------------------------------===//
// Types.
//===----------------------------------------------------------------------===//

MLIR_CAPI_EXPORTED bool simTypeIsAFormatString(MlirType type);
MLIR_CAPI_EXPORTED MlirType simFormatStringTypeGet(MlirContext ctx);

MLIR_CAPI_EXPORTED bool simTypeIsADynamicString(MlirType type);
MLIR_CAPI_EXPORTED MlirType simDynamicStringTypeGet(MlirContext ctx);

MLIR_CAPI_EXPORTED bool simTypeIsAQueue(MlirType type);
MLIR_CAPI_EXPORTED MlirType simQueueTypeGet(MlirType elementType,
                                            unsigned bound);
MLIR_CAPI_EXPORTED MlirType simQueueTypeGetElementType(MlirType type);
MLIR_CAPI_EXPORTED unsigned simQueueTypeGetBound(MlirType type);

MLIR_CAPI_EXPORTED bool simTypeIsAAssocArray(MlirType type);
MLIR_CAPI_EXPORTED MlirType simAssocArrayTypeGet(MlirType elementType,
                                                 MlirType indexType);
MLIR_CAPI_EXPORTED MlirType simAssocArrayTypeGetElementType(MlirType type);
MLIR_CAPI_EXPORTED MlirType simAssocArrayTypeGetIndexType(MlirType type);

typedef enum {
  SIM_DPI_DIRECTION_INPUT = 0,
  SIM_DPI_DIRECTION_OUTPUT = 1,
  SIM_DPI_DIRECTION_INOUT = 2,
  SIM_DPI_DIRECTION_RETURN = 3,
  SIM_DPI_DIRECTION_REF = 4,
} SimDPIDirection;

typedef struct {
  MlirStringRef name;
  MlirType type;
  SimDPIDirection direction;
} SimDPIArgument;

MLIR_CAPI_EXPORTED bool simTypeIsADPIFunction(MlirType type);
MLIR_CAPI_EXPORTED MlirType simDPIFunctionTypeGet(
    MlirContext ctx, intptr_t numArguments, const SimDPIArgument *arguments);
MLIR_CAPI_EXPORTED intptr_t simDPIFunctionTypeGetNumArguments(MlirType type);
/// The returned name refers to storage owned by the type's context.
MLIR_CAPI_EXPORTED SimDPIArgument simDPIFunctionTypeGetArgument(MlirType type,
                                                                intptr_t index);
MLIR_CAPI_EXPORTED MlirType simDPIFunctionTypeGetFunctionType(MlirType type);

MLIR_CAPI_EXPORTED bool simTypeIsAOutputStream(MlirType type);
MLIR_CAPI_EXPORTED MlirType simOutputStreamTypeGet(MlirContext ctx);

#ifdef __cplusplus
}
#endif

#endif // CIRCT_C_DIALECT_SIM_H
