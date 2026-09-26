//===- sim.c - Sim dialect C API tests
//-------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// RUN: circt-capi-sim-test

#include "circt-c/Dialect/Sim.h"
#include "mlir-c/BuiltinTypes.h"

#include <assert.h>
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

int main(void) {
  MlirContext ctx = mlirContextCreate();
  mlirDialectHandleLoadDialect(mlirGetDialectHandle__sim__(), ctx);
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
  assert(mlirLogicalResultIsSuccess(
      mlirExportDPIInterface(module, appendSchema, &schema)));
  assert(strstr(schema.text, "\"type\": \"shortreal\"") != NULL);
  assert(strstr(schema.text, "\"type\": \"real\"") != NULL);
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

  mlirContextDestroy(ctx);
  return 0;
}
