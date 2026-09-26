//===- ExportDPIInterfaceTest.cpp - DPI ABI tests
//--------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Sim/ExportDPIInterface.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/Moore/MooreDialect.h"
#include "circt/Dialect/SV/SVDialect.h"
#include "circt/Dialect/Sim/SimDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

using namespace mlir;
using namespace circt;

namespace {

class ExportDPIInterfaceTest : public testing::Test {
protected:
  ExportDPIInterfaceTest() {
    context.loadDialect<sim::SimDialect, hw::HWDialect, moore::MooreDialect,
                        sv::SVDialect, LLVM::LLVMDialect>();
  }

  void expectSchema(llvm::StringRef ir, llvm::StringRef expectedJSON) {
    auto module = parseSourceString<ModuleOp>(ir, &context);
    ASSERT_TRUE(module);
    std::string output;
    llvm::raw_string_ostream stream(output);
    ASSERT_TRUE(succeeded(sim::exportDPIInterface(*module, stream)));

    auto actual = llvm::json::parse(output);
    ASSERT_TRUE(bool(actual)) << llvm::toString(actual.takeError());
    auto expected = llvm::json::parse(expectedJSON);
    ASSERT_TRUE(bool(expected)) << llvm::toString(expected.takeError());
    EXPECT_EQ(*actual, *expected);
  }

  void expectFailure(llvm::StringRef ir, llvm::StringRef expectedDiagnostic) {
    auto module = parseSourceString<ModuleOp>(ir, &context);
    ASSERT_TRUE(module);
    std::string diagnostic;
    ScopedDiagnosticHandler handler(&context, [&](Diagnostic &diag) {
      llvm::raw_string_ostream stream(diagnostic);
      diag.print(stream);
      return success();
    });

    std::string output = "existing output";
    llvm::raw_string_ostream stream(output);
    EXPECT_TRUE(failed(sim::exportDPIInterface(*module, stream)));
    EXPECT_EQ(output, "existing output");
    EXPECT_NE(diagnostic.find(expectedDiagnostic.str()), std::string::npos)
        << diagnostic;
  }

  MLIRContext context;
};

TEST_F(ExportDPIInterfaceTest, NamesDirectionsAndSignedness) {
  expectSchema(R"mlir(
    module {
      sim.func.dpi @step(in %cycle: i32, out drive: ui8,
                        inout %state: si16, return status: si32)
          attributes {verilogName = "dpi_step"}
    }
  )mlir",
               R"json({
    "dpi_functions": [{
      "function": "dpi_step",
      "arguments": [
        {"name": "cycle", "direction": "in", "width": 32,
         "signed": false},
        {"name": "drive", "direction": "out", "width": 8,
         "signed": false},
        {"name": "state", "direction": "inout", "width": 16,
         "signed": true},
        {"name": "status", "direction": "return", "width": 32,
         "signed": true}
      ]
    }]
  })json");
}

TEST_F(ExportDPIInterfaceTest, WidthsAndFunctionOrder) {
  expectSchema(R"mlir(
    module {
      sim.func.dpi @tick(in %edge: i1)
      sim.func.dpi @read(out value: i7, in %byte: i8, in %half: i16,
                        in %word: i32, in %wide: i63, return status: i64)
      sim.func.dpi @finish()
    }
  )mlir",
               R"json({
    "dpi_functions": [
      {"function": "tick", "arguments": [
        {"name": "edge", "direction": "in", "width": 1,
         "signed": false}
      ]},
      {"function": "read", "arguments": [
        {"name": "value", "direction": "out", "width": 7,
         "signed": false},
        {"name": "byte", "direction": "in", "width": 8,
         "signed": false},
        {"name": "half", "direction": "in", "width": 16,
         "signed": false},
        {"name": "word", "direction": "in", "width": 32,
         "signed": false},
        {"name": "wide", "direction": "in", "width": 63,
         "signed": false},
        {"name": "status", "direction": "return", "width": 64,
         "signed": false}
      ]},
      {"function": "finish", "arguments": []}
    ]
  })json");
}

TEST_F(ExportDPIInterfaceTest, DoesNotModifyIR) {
  auto module = parseSourceString<ModuleOp>(R"mlir(
    module {
      sim.func.dpi @step(in %cycle: i32, in %sample: f32,
                        in %data: !moore.l1024, return result: f64)
    }
  )mlir",
                                            &context);
  ASSERT_TRUE(module);
  std::string before;
  llvm::raw_string_ostream beforeStream(before);
  module->print(beforeStream);

  std::string output;
  llvm::raw_string_ostream stream(output);
  ASSERT_TRUE(succeeded(sim::exportDPIInterface(*module, stream)));
  std::string after;
  llvm::raw_string_ostream afterStream(after);
  module->print(afterStream);
  EXPECT_EQ(before, after);

  std::string repeatedOutput;
  llvm::raw_string_ostream repeatedStream(repeatedOutput);
  ASSERT_TRUE(succeeded(sim::exportDPIInterface(*module, repeatedStream)));
  EXPECT_EQ(output, repeatedOutput);
}

TEST_F(ExportDPIInterfaceTest, RejectsMissingFunctions) {
  expectFailure("module {}", "no sim.func.dpi operations to export");
}

TEST_F(ExportDPIInterfaceTest, ExportsWideIntegers) {
  expectSchema(R"mlir(
    module {
      sim.func.dpi @transfer(in %address: i65, in %data: i1024,
                            out response: si1024)
    }
  )mlir",
               R"json({
    "dpi_functions": [{
      "function": "transfer",
      "arguments": [
        {"name": "address", "direction": "in", "width": 65,
         "signed": false},
        {"name": "data", "direction": "in", "width": 1024,
         "signed": false},
        {"name": "response", "direction": "out", "width": 1024,
         "signed": true}
      ]
    }]
  })json");
}

TEST_F(ExportDPIInterfaceTest, ExportsFloatingPointArguments) {
  expectSchema(R"mlir(
    module {
      sim.func.dpi @sample(in %value: f32, out result: f64,
                          inout %state: f32, return status: f64)
    }
  )mlir",
               R"json({
    "dpi_functions": [{
      "function": "sample",
      "arguments": [
        {"name": "value", "direction": "in", "type": "shortreal"},
        {"name": "result", "direction": "out", "type": "real"},
        {"name": "state", "direction": "inout", "type": "shortreal"},
        {"name": "status", "direction": "return", "type": "real"}
      ]
    }]
  })json");
}

TEST_F(ExportDPIInterfaceTest, ExportsStringsAndChandles) {
  expectSchema(R"mlir(
    module {
      sim.func.dpi @send(in %message: !hw.string, in %handle: !moore.chandle,
                        out buffer: !moore.chandle, return status: i32)
    }
  )mlir",
               R"json({
    "dpi_functions": [{
      "function": "send",
      "arguments": [
        {"name": "message", "direction": "in", "type": "string"},
        {"name": "handle", "direction": "in", "type": "chandle"},
        {"name": "buffer", "direction": "out", "type": "chandle"},
        {"name": "status", "direction": "return", "width": 32,
         "signed": false}
      ]
    }]
  })json");
}

TEST_F(ExportDPIInterfaceTest, ExportsAggregateTypes) {
  expectSchema(R"mlir(
    module {
      sim.func.dpi @aggregate(in %packed: !hw.array<4xi8>,
          out unpacked: !hw.uarray<2xi16>,
          in %record: !hw.struct<data: i1024, tag: i8>,
          in %choice: !hw.union<byte: i8, word: i32>,
          in %mode: !hw.enum<Idle, Active>)
    }
  )mlir",
               R"json({
    "dpi_functions": [{
      "function": "aggregate",
      "arguments": [
        {"name": "packed", "direction": "in", "type": "array",
         "packed": true, "size": 4, "element": {"width": 8, "signed": false}},
        {"name": "unpacked", "direction": "out", "type": "array",
         "packed": false, "size": 2, "element": {"width": 16, "signed": false}},
        {"name": "record", "direction": "in",
         "type": "struct", "packed": true, "members": [
           {"name": "data", "width": 1024, "signed": false},
           {"name": "tag", "width": 8, "signed": false}]},
        {"name": "choice", "direction": "in",
         "type": "union", "packed": true, "members": [
           {"name": "byte", "width": 8, "signed": false, "offset": 0},
           {"name": "word", "width": 32, "signed": false, "offset": 0}]},
        {"name": "mode", "direction": "in", "type": "enum", "width": 1,
         "signed": false, "members": ["Idle", "Active"]}
      ]
    }]
  })json");
}

TEST_F(ExportDPIInterfaceTest, RejectsZeroWidthWithoutPartialOutput) {
  expectFailure(R"mlir(
    module {
      sim.func.dpi @valid(in %cycle: i32)
      sim.func.dpi @step(in %value: i0)
    }
  )mlir",
                "unsupported DPI schema argument \"value\"");
}

TEST_F(ExportDPIInterfaceTest, ExportsFourStateAndMooreScalarTypes) {
  expectSchema(R"mlir(
    module {
      sim.func.dpi @scalar(in %data: !moore.l1024, in %time: !moore.time,
          in %message: !moore.string, in %small: !moore.f32,
          in %large: !moore.f64, return flag: !moore.l1)
    }
  )mlir",
               R"json({
    "dpi_functions": [{"function": "scalar", "arguments": [
      {"name": "data", "direction": "in", "type": "logic",
       "width": 1024, "signed": false},
      {"name": "time", "direction": "in", "type": "time"},
      {"name": "message", "direction": "in", "type": "string"},
      {"name": "small", "direction": "in", "type": "shortreal"},
      {"name": "large", "direction": "in", "type": "real"},
      {"name": "flag", "direction": "return", "type": "logic",
       "width": 1, "signed": false}
    ]}]
  })json");
}

TEST_F(ExportDPIInterfaceTest, ExportsOpenArraysAndNestedAggregates) {
  expectSchema(R"mlir(
    module {
      sim.func.dpi @arrays(in %data: !sv.open_uarray<i1024>,
          inout %records: !moore.open_uarray<ustruct<{value: f64, label: string}>>,
          out samples: !moore.uarray<2 x f32>,
          in %packed: !moore.open_array<l1>)
    }
  )mlir",
               R"json({
    "dpi_functions": [{"function": "arrays", "arguments": [
      {"name": "data", "direction": "in", "type": "array", "packed": false,
       "size": null, "element": {"width": 1024, "signed": false}},
      {"name": "records", "direction": "inout", "type": "array", "packed": false,
       "size": null, "element": {"type": "struct", "packed": false, "members": [
         {"name": "value", "type": "real"},
         {"name": "label", "type": "string"}]}},
      {"name": "samples", "direction": "out", "type": "array", "packed": false,
       "size": 2, "element": {"type": "shortreal"}},
      {"name": "packed", "direction": "in", "type": "array", "packed": true,
       "size": null, "element": {"type": "logic", "width": 1, "signed": false}}
    ]}]
  })json");
}

TEST_F(ExportDPIInterfaceTest, ExportsAliasesByUnderlyingType) {
  expectSchema(R"mlir(
    module {
      sim.func.dpi @alias(in %value: !hw.typealias<@Types::@Word, si32>)
    }
  )mlir",
               R"json({
    "dpi_functions": [{"function": "alias", "arguments": [
      {"name": "value", "direction": "in", "width": 32, "signed": true}
    ]}]
  })json");
}

TEST_F(ExportDPIInterfaceTest, ExportsMoorePackedAggregates) {
  expectSchema(R"mlir(
    module {
      sim.func.dpi @packed(in %data: !moore.array<2 x l8>,
          in %record: !moore.struct<{data: l8}>,
          in %choice: !moore.union<{a: i8, b: l8}>)
    }
  )mlir",
               R"json({
    "dpi_functions": [{"function": "packed", "arguments": [
      {"name": "data", "direction": "in", "type": "array", "packed": true,
       "size": 2, "element": {"type": "logic", "width": 8, "signed": false}},
      {"name": "record", "direction": "in", "type": "struct", "packed": true,
       "members": [{"name": "data", "type": "logic", "width": 8, "signed": false}]},
      {"name": "choice", "direction": "in", "type": "union", "packed": true,
       "members": [{"name": "a", "width": 8, "signed": false},
                   {"name": "b", "type": "logic", "width": 8, "signed": false}]}
    ]}]
  })json");
}

TEST_F(ExportDPIInterfaceTest, RejectsRefArguments) {
  expectFailure(R"mlir(
    module {
      sim.func.dpi @valid(in %cycle: i32)
      sim.func.dpi @invalid(ref %buffer: !llvm.ptr)
    }
  )mlir",
                "IEEE 1800 DPI does not allow ref arguments");
}

TEST_F(ExportDPIInterfaceTest, RejectsAmbiguousPointers) {
  expectFailure("module { sim.func.dpi @pointer(in %value: !llvm.ptr) }",
                "unsupported DPI schema argument \"value\"");
}

TEST_F(ExportDPIInterfaceTest, RejectsNonDPITypes) {
  for (const char *type : {"f16", "bf16", "f80", "index", "!moore.event",
                           "!moore.queue<i8, 4>", "!moore.uunion<{a: i8}>"}) {
    SCOPED_TRACE(type);
    expectFailure((llvm::Twine("module { sim.func.dpi @invalid(in %value: ") +
                   type + ") }")
                      .str(),
                  "unsupported DPI schema argument \"value\"");
  }
}

TEST_F(ExportDPIInterfaceTest, RejectsInvalidReturnTypes) {
  for (const char *type :
       {"i7", "i1024", "!moore.l32", "!moore.time", "!hw.array<2xi8>",
        "!sv.open_uarray<i32>", "!hw.struct<data: i32>", "!hw.enum<A, B>"}) {
    SCOPED_TRACE(type);
    expectFailure(
        (llvm::Twine("module { sim.func.dpi @invalid(return result: ") + type +
         ") }")
            .str(),
        "unsupported DPI schema argument \"result\"");
  }
}

TEST_F(ExportDPIInterfaceTest, RejectsUnsupportedNestedMembers) {
  expectFailure(R"mlir(
    module {
      sim.func.dpi @nested(in %value: !moore.ustruct<{data: queue<i8, 4>}>)
    }
  )mlir",
                "unsupported DPI schema argument \"value\"");
}

} // namespace
