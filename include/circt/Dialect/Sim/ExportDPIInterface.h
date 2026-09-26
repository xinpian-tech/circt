//===- ExportDPIInterface.h - Export DPI function ABIs -----------*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_SIM_EXPORTDPIINTERFACE_H
#define CIRCT_DIALECT_SIM_EXPORTDPIINTERFACE_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Support/LogicalResult.h"

namespace llvm {
class raw_ostream;
} // namespace llvm

namespace circt::sim {

/// Export DPI declarations without modifying the IR. Validate types, argument
/// directions, and results against IEEE 1800-2023 Clause 35 and Annex H.
/// Type descriptions use SV names and recursive aggregate descriptions, not
/// MLIR type spellings. See docs/Dialects/Sim/ExportDPIInterface.md
/// for the JSON schema.
/// This does not validate support in downstream lowering.
/// No bytes are written on failure.
mlir::LogicalResult exportDPIInterface(mlir::ModuleOp module,
                                       llvm::raw_ostream &output);

} // namespace circt::sim

#endif // CIRCT_DIALECT_SIM_EXPORTDPIINTERFACE_H
