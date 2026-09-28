/*===- link-circuits.c - LinkCircuits C API test --------------------------===*\
|*                                                                            *|
|* Part of the LLVM Project, under the Apache License v2.0 with LLVM          *|
|* Exceptions.                                                                *|
|* See https://llvm.org/LICENSE.txt for license information.                  *|
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception                    *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

/* RUN: circt-capi-link-circuits-test 2>&1 | FileCheck %s
 */

#include "circt-c/Dialect/FIRRTL.h"
#include "mlir-c/BuiltinAttributes.h"
#include "mlir-c/IR.h"
#include "mlir-c/Pass.h"
#include "mlir-c/Support.h"
#include <assert.h>
#include <stdio.h>

static bool stringEquals(MlirStringRef actual, const char *expected) {
  return mlirStringRefEqual(actual, mlirStringRefCreateFromCString(expected));
}

static bool hasModule(MlirOperation circuit, const char *name) {
  MlirBlock body = mlirRegionGetFirstBlock(mlirOperationGetRegion(circuit, 0));
  for (MlirOperation op = mlirBlockGetFirstOperation(body);
       !mlirOperationIsNull(op); op = mlirOperationGetNextInBlock(op)) {
    MlirAttribute symbol = mlirOperationGetAttributeByName(
        op, mlirStringRefCreateFromCString("sym_name"));
    if (!mlirAttributeIsNull(symbol) &&
        stringEquals(mlirStringAttrGetValue(symbol), name))
      return true;
  }
  return false;
}

static void testLink(MlirContext ctx, bool noMangle) {
  const char *input = "module {\n"
                      "  firrtl.circuit \"Base\" {\n"
                      "    firrtl.module @Base() {}\n"
                      "    firrtl.module private @Helper() {}\n"
                      "  }\n"
                      "  firrtl.circuit \"Other\" {\n"
                      "    firrtl.module @Other() {}\n"
                      "  }\n"
                      "}\n";
  MlirModule module =
      mlirModuleCreateParse(ctx, mlirStringRefCreateFromCString(input));
  assert(!mlirModuleIsNull(module));

  MlirPassManager pm = mlirPassManagerCreate(ctx);
  mlirPassManagerAddOwnedPass(
      pm, circtFirrtlCreateLinkCircuitsPass(
              mlirStringRefCreateFromCString("Other"), noMangle));
  MlirLogicalResult result =
      mlirPassManagerRunOnOp(pm, mlirModuleGetOperation(module));
  assert(mlirLogicalResultIsSuccess(result));

  MlirBlock body = mlirModuleGetBody(module);
  MlirOperation circuit = mlirBlockGetFirstOperation(body);
  assert(!mlirOperationIsNull(circuit));
  assert(mlirOperationIsNull(mlirOperationGetNextInBlock(circuit)));
  MlirAttribute circuitName = mlirOperationGetAttributeByName(
      circuit, mlirStringRefCreateFromCString("name"));
  assert(stringEquals(mlirStringAttrGetValue(circuitName), "Other"));
  assert(hasModule(circuit, "Base"));
  assert(hasModule(circuit, "Other"));
  assert(hasModule(circuit, noMangle ? "Helper" : "Base_Helper"));
  printf("linked two circuits with noMangle=%d\n", noMangle);
  // CHECK: linked two circuits with noMangle=0
  // CHECK: linked two circuits with noMangle=1

  mlirPassManagerDestroy(pm);
  mlirModuleDestroy(module);
}

static void testFailure(MlirContext ctx) {
  const char *input = "module {\n"
                      "  firrtl.circuit \"First\" {\n"
                      "    firrtl.module @First() {}\n"
                      "    firrtl.module @Shared() {}\n"
                      "  }\n"
                      "  firrtl.circuit \"Second\" {\n"
                      "    firrtl.module @Second() {}\n"
                      "    firrtl.module @Shared(in %in: !firrtl.uint<1>) {}\n"
                      "  }\n"
                      "}\n";
  MlirModule module =
      mlirModuleCreateParse(ctx, mlirStringRefCreateFromCString(input));
  assert(!mlirModuleIsNull(module));

  MlirPassManager pm = mlirPassManagerCreate(ctx);
  mlirPassManagerAddOwnedPass(
      pm, circtFirrtlCreateLinkCircuitsPass(
              mlirStringRefCreateFromCString("First"), true));
  MlirLogicalResult result =
      mlirPassManagerRunOnOp(pm, mlirModuleGetOperation(module));
  assert(mlirLogicalResultIsFailure(result));
  printf("link failure returned through MLIR pass manager\n");
  // CHECK: link failure returned through MLIR pass manager

  mlirPassManagerDestroy(pm);
  mlirModuleDestroy(module);
}

int main(void) {
  MlirContext ctx = mlirContextCreate();
  mlirDialectHandleLoadDialect(mlirGetDialectHandle__firrtl__(), ctx);
  testLink(ctx, false);
  testLink(ctx, true);
  testFailure(ctx);
  mlirContextDestroy(ctx);
  return 0;
}
