//===- sim.c - Sim Dialect C API tests ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// RUN: circt-capi-sim-test 2>&1 | FileCheck %s

#include "circt-c/Dialect/Sim.h"
#include "mlir-c/BuiltinTypes.h"
#include "mlir-c/Dialect/LLVM.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

struct SchemaBuffer {
  char text[4096];
  size_t length;
};

static void appendSchema(MlirStringRef chunk, void *userData) {
  struct SchemaBuffer *buffer = userData;
  assert(buffer->length < sizeof(buffer->text));
  assert(chunk.length < sizeof(buffer->text) - buffer->length);
  memcpy(buffer->text + buffer->length, chunk.data, chunk.length);
  buffer->length += chunk.length;
  buffer->text[buffer->length] = '\0';
}

static void testEmptySignature(MlirContext ctx) {
  // CHECK-LABEL: Empty signature
  fprintf(stderr, "Empty signature\n");

  MlirType type = simDPIFunctionTypeGet(ctx, 0, NULL);
  // CHECK-NEXT: is DPI function: 1
  fprintf(stderr, "is DPI function: %d\n", simTypeIsADPIFunction(type));
  // CHECK-NEXT: arguments: 0
  fprintf(stderr, "arguments: %" PRIdPTR "\n",
          simDPIFunctionTypeGetNumArguments(type));
  // CHECK-NEXT: context matches: 1
  fprintf(stderr, "context matches: %d\n",
          mlirContextEqual(mlirTypeGetContext(type), ctx));
  // CHECK-NEXT: uniqued: 1
  fprintf(stderr, "uniqued: %d\n",
          mlirTypeEqual(type, simDPIFunctionTypeGet(ctx, 0, NULL)));
  // CHECK-NEXT: !sim.dpi_functy<>
  mlirTypeDump(type);

  MlirType callType = simDPIFunctionTypeGetFunctionType(type);
  // CHECK-NEXT: is function: 1
  fprintf(stderr, "is function: %d\n", mlirTypeIsAFunction(callType));
  // CHECK-NEXT: inputs: 0
  fprintf(stderr, "inputs: %" PRIdPTR "\n",
          mlirFunctionTypeGetNumInputs(callType));
  // CHECK-NEXT: results: 0
  fprintf(stderr, "results: %" PRIdPTR "\n",
          mlirFunctionTypeGetNumResults(callType));
  // CHECK-NEXT: call context matches: 1
  fprintf(stderr, "call context matches: %d\n",
          mlirContextEqual(mlirTypeGetContext(callType), ctx));
  // CHECK-NEXT: () -> ()
  mlirTypeDump(callType);
  // CHECK-NEXT: function is DPI function: 0
  fprintf(stderr, "function is DPI function: %d\n",
          simTypeIsADPIFunction(callType));
  // CHECK-NEXT: integer is DPI function: 0
  fprintf(stderr, "integer is DPI function: %d\n",
          simTypeIsADPIFunction(mlirIntegerTypeGet(ctx, 32)));
}

static void testArgumentsAndCallSignature(MlirContext ctx) {
  // CHECK-LABEL: Arguments and call signature
  fprintf(stderr, "Arguments and call signature\n");

  MlirType i7 = mlirIntegerTypeGet(ctx, 7);
  MlirType i32 = mlirIntegerTypeGet(ctx, 32);
  MlirType i64 = mlirIntegerTypeGet(ctx, 64);
  MlirType i1024 = mlirIntegerTypeGet(ctx, 1024);
  MlirType ptr = mlirLLVMPointerTypeGet(ctx, 0);
  SimDPIArgument arguments[] = {
      {mlirStringRefCreateFromCString("value"), i7, SIM_DPI_DIRECTION_OUTPUT},
      {mlirStringRefCreateFromCString("cycle"), i32, SIM_DPI_DIRECTION_INPUT},
      {mlirStringRefCreateFromCString("state"), i1024, SIM_DPI_DIRECTION_INOUT},
      {mlirStringRefCreateFromCString("data"), ptr, SIM_DPI_DIRECTION_REF},
      {mlirStringRefCreateFromCString("status"), i64, SIM_DPI_DIRECTION_RETURN},
  };
  intptr_t count = sizeof(arguments) / sizeof(arguments[0]);
  MlirType type = simDPIFunctionTypeGet(ctx, count, arguments);
  // CHECK-NEXT: is DPI function: 1
  fprintf(stderr, "is DPI function: %d\n", simTypeIsADPIFunction(type));
  // CHECK-NEXT: arguments: 5
  fprintf(stderr, "arguments: %" PRIdPTR "\n",
          simDPIFunctionTypeGetNumArguments(type));

  // CHECK-NEXT: argument 0: name matches 1, type matches 1, direction matches 1
  // CHECK-NEXT: argument 1: name matches 1, type matches 1, direction matches 1
  // CHECK-NEXT: argument 2: name matches 1, type matches 1, direction matches 1
  // CHECK-NEXT: argument 3: name matches 1, type matches 1, direction matches 1
  // CHECK-NEXT: argument 4: name matches 1, type matches 1, direction matches 1
  for (intptr_t i = 0; i < count; ++i) {
    SimDPIArgument argument = simDPIFunctionTypeGetArgument(type, i);
    fprintf(stderr,
            "argument %" PRIdPTR
            ": name matches %d, type matches %d, direction matches %d\n",
            i, mlirStringRefEqual(argument.name, arguments[i].name),
            mlirTypeEqual(argument.type, arguments[i].type),
            argument.direction == arguments[i].direction);
  }

  // CHECK-NEXT: !sim.dpi_functy<out "value" : i7, in "cycle" : i32,
  // CHECK-SAME: inout "state" : i1024, ref "data" : !llvm.ptr,
  // CHECK-SAME: return "status" : i64>
  mlirTypeDump(type);

  MlirType callType = simDPIFunctionTypeGetFunctionType(type);
  // CHECK-NEXT: is function: 1
  fprintf(stderr, "is function: %d\n", mlirTypeIsAFunction(callType));
  // CHECK-NEXT: inputs: 3
  fprintf(stderr, "inputs: %" PRIdPTR "\n",
          mlirFunctionTypeGetNumInputs(callType));
  // CHECK-NEXT: results: 3
  fprintf(stderr, "results: %" PRIdPTR "\n",
          mlirFunctionTypeGetNumResults(callType));

  // CHECK-NEXT: input 0: i32
  // CHECK-NEXT: result 0: i7
  // CHECK-NEXT: input 1: i1024
  // CHECK-NEXT: result 1: i1024
  // CHECK-NEXT: input 2: !llvm.ptr
  // CHECK-NEXT: result 2: i64
  for (intptr_t i = 0; i < 3; ++i) {
    fprintf(stderr, "input %" PRIdPTR ": ", i);
    mlirTypeDump(mlirFunctionTypeGetInput(callType, i));
    fprintf(stderr, "result %" PRIdPTR ": ", i);
    mlirTypeDump(mlirFunctionTypeGetResult(callType, i));
  }

  // CHECK-NEXT: (i32, i1024, !llvm.ptr) -> (i7, i1024, i64)
  mlirTypeDump(callType);
}

static void testTypes(MlirContext ctx) {
  MlirType i32 = mlirIntegerTypeGet(ctx, 32);
  MlirType i8 = mlirIntegerTypeGet(ctx, 8);

  MlirType format = simFormatStringTypeGet(ctx);
  MlirType dynamic = simDynamicStringTypeGet(ctx);
  MlirType stream = simOutputStreamTypeGet(ctx);
  assert(simTypeIsAFormatString(format));
  assert(simTypeIsADynamicString(dynamic));
  assert(simTypeIsAOutputStream(stream));
  assert(!simTypeIsAQueue(format));

  MlirType queue = simQueueTypeGet(i8, 4);
  assert(simTypeIsAQueue(queue));
  assert(mlirTypeEqual(simQueueTypeGetElementType(queue), i8));
  assert(simQueueTypeGetBound(queue) == 4);

  MlirType assoc = simAssocArrayTypeGet(i32, i8);
  assert(simTypeIsAAssocArray(assoc));
  assert(mlirTypeEqual(simAssocArrayTypeGetElementType(assoc), i32));
  assert(mlirTypeEqual(simAssocArrayTypeGetIndexType(assoc), i8));

  SimDPIArgument arguments[] = {
      {mlirStringRefCreateFromCString("input"), i32, SIM_DPI_DIRECTION_INPUT},
      {mlirStringRefCreateFromCString("output"), i8, SIM_DPI_DIRECTION_OUTPUT},
      {mlirStringRefCreateFromCString("result"), i32, SIM_DPI_DIRECTION_RETURN},
  };
  MlirType dpi = simDPIFunctionTypeGet(ctx, 3, arguments);
  assert(simTypeIsADPIFunction(dpi));
  assert(simDPIFunctionTypeGetNumArguments(dpi) == 3);
  SimDPIArgument result = simDPIFunctionTypeGetArgument(dpi, 2);
  assert(result.name.length == 6);
  assert(memcmp(result.name.data, "result", 6) == 0);
  assert(mlirTypeEqual(result.type, i32));
  assert(result.direction == SIM_DPI_DIRECTION_RETURN);
  MlirType function = simDPIFunctionTypeGetFunctionType(dpi);
  assert(mlirFunctionTypeGetNumInputs(function) == 1);
  assert(mlirFunctionTypeGetNumResults(function) == 2);

  MlirType emptyDpi = simDPIFunctionTypeGet(ctx, 0, NULL);
  assert(simDPIFunctionTypeGetNumArguments(emptyDpi) == 0);
  assert(mlirFunctionTypeGetNumInputs(
             simDPIFunctionTypeGetFunctionType(emptyDpi)) == 0);
}

static void testExportDPIInterface(MlirContext ctx) {
  MlirModule module = mlirModuleCreateParse(
      ctx, mlirStringRefCreateFromCString(
               "module { "
               "sim.func.dpi @step(in %cycle: i32, out drive: i8, "
               "return status: i32) attributes {verilogName = \"dpi_step\"} "
               "sim.func.dpi @tick(in %edge: i1) "
               "sim.func.dpi @read(out value: i7, return status: i64) "
               "}"));
  assert(!mlirModuleIsNull(module));
  struct SchemaBuffer schema = {{0}, 0};
  assert(mlirLogicalResultIsSuccess(
      mlirExportDPIInterface(module, appendSchema, &schema)));
  assert(strstr(schema.text, "\"dpi_functions\"") != NULL);
  const char *step = strstr(schema.text, "\"function\": \"dpi_step\"");
  const char *tick = strstr(schema.text, "\"function\": \"tick\"");
  const char *read = strstr(schema.text, "\"function\": \"read\"");
  assert(step && tick && read && step < tick && tick < read);
  assert(strstr(schema.text, "\"direction\": \"return\"") != NULL);
  assert(strstr(schema.text, "\"width\": 1\n") != NULL);
  assert(strstr(schema.text, "\"width\": 7") != NULL);
  assert(strstr(schema.text, "\"width\": 32") != NULL);
  assert(strstr(schema.text, "\"signed\": true") != NULL);
  assert(strstr(schema.text, "\"signed\": false") != NULL);
  assert(strstr(schema.text, "\"dut\"") == NULL);
  schema.length = 0;
  schema.text[0] = '\0';
  assert(mlirLogicalResultIsSuccess(
      mlirExportDPIInterface(module, appendSchema, &schema)));
  assert(strstr(schema.text, "\"dpi_functions\"") != NULL);
  mlirModuleDestroy(module);

  module = mlirModuleCreateParse(
      ctx, mlirStringRefCreateFromCString(
               "module { sim.func.dpi @step(in %address: i65, "
               "out data: i1024) }"));
  assert(!mlirModuleIsNull(module));
  schema.length = 0;
  schema.text[0] = '\0';
  assert(mlirLogicalResultIsSuccess(
      mlirExportDPIInterface(module, appendSchema, &schema)));
  assert(strstr(schema.text, "\"width\": 65") != NULL);
  assert(strstr(schema.text, "\"width\": 1024") != NULL);
  mlirModuleDestroy(module);

  module = mlirModuleCreateParse(
      ctx, mlirStringRefCreateFromCString(
               "module { sim.func.dpi @step(in %value: f32, "
               "return result: f64) }"));
  assert(!mlirModuleIsNull(module));
  schema.length = 0;
  schema.text[0] = '\0';
  assert(mlirLogicalResultIsFailure(
      mlirExportDPIInterface(module, appendSchema, &schema)));
  assert(schema.length == 0);
  mlirModuleDestroy(module);

  module =
      mlirModuleCreateParse(ctx, mlirStringRefCreateFromCString("module {}"));
  assert(!mlirModuleIsNull(module));
  schema.length = 0;
  schema.text[0] = '\0';
  assert(mlirLogicalResultIsFailure(
      mlirExportDPIInterface(module, appendSchema, &schema)));
  assert(schema.length == 0);
  mlirModuleDestroy(module);
}

int main(void) {
  MlirContext ctx = mlirContextCreate();
  MlirDialect sim =
      mlirDialectHandleLoadDialect(mlirGetDialectHandle__sim__(), ctx);
  MlirDialect llvm =
      mlirDialectHandleLoadDialect(mlirGetDialectHandle__llvm__(), ctx);
  assert(!mlirDialectIsNull(sim));
  assert(!mlirDialectIsNull(llvm));

  testTypes(ctx);
  testEmptySignature(ctx);
  testArgumentsAndCallSignature(ctx);
  testExportDPIInterface(ctx);

  mlirContextDestroy(ctx);
  return 0;
}
