# Sim DPI Interface JSON

`circt::sim::exportDPIInterface` describes imported `sim.func.dpi`
declarations. The C API `mlirExportDPIInterface` delivers the same JSON through
a callback. Neither interface changes the IR or performs SV lowering.

The JSON format is specific to CIRCT. Its type and direction rules follow
[IEEE 1800-2023](https://standards.ieee.org/ieee/1800/7743/), Clause 35 and
Annex H; IEEE does not define a DPI JSON format.

## Functions and Arguments

The root object contains `dpi_functions`, an array in declaration order.
Each function has `function` (the `verilogName`, or the symbol name when absent)
and `arguments`, in signature order. An empty argument array is valid.

Each argument contains `name`, `direction`, and a type description. `direction`
is `in`, `out`, `inout`, or `return`. The last identifies the function result,
not a C parameter. No `return` entry means the function returns `void`.
Standard DPI does not allow `ref` arguments.

## Type Descriptions

Type descriptions are JSON objects. Their fields appear directly in each
argument and are also used recursively for aggregate elements and members.

| Type | Description fields |
| --- | --- |
| Two-state integer | `width`, `signed`; no `type` field |
| Four-state vector | `type: "logic"`, `width`, `signed` |
| Four-state time | `type: "time"` |
| Floating point | `type: "shortreal"` or `type: "real"` |
| String | `type: "string"` |
| C handle | `type: "chandle"` |
| Array | `type: "array"`, `packed`, `size`, `element` |
| Structure | `type: "struct"`, `packed`, `members` |
| Packed union | `type: "union"`, `packed: true`, `members` |
| Enumeration | `type: "enum"`, `width`, `signed`, `members` |

Integer widths are not limited to 64 bits. For two-state integers, widths
8/16/32/64 use the native SV integer atom convention of the Verilog exporter;
width 1 denotes `bit`, and other widths denote packed bit vectors. Packed array
elements remain packed integral types. MLIR signless integers are described with
`signed: false`; callers must supply signed integer types when required.

Four-state information, explicit handles, and unpacked structures can be
expressed using existing Moore types. Moore integer types have no signedness
information, so their descriptions use `signed: false`.

Floating-point descriptions accept builtin `f32`/`f64` and Moore real types,
not arbitrary MLIR floating-point formats. Strings accept HW, Sim dynamic
string, and Moore string types. C handles require explicit Moore chandle types:
an opaque LLVM pointer cannot distinguish a chandle from an open-array handle
and is rejected rather than guessed.

An array's `size` is its element count, or `null` for an open dimension.
`element` is another type description. Nested array descriptions preserve the
dimensions; fixed dimensions use the canonical ranges implied by the source IR.
Packed elements must be integral. Arrays accept HW/Moore fixed arrays and
SV/Moore open arrays.

Structure and union `members` are ordered type descriptions with an additional
`name`. HW union members also include their bit `offset`, preserving the HW
layout (including any padding required for SV emission). Packed structure
members follow SV declaration order, with the first member at the most
significant end. Unpacked structures retain member order without assuming C
padding. HW enums have sequential values starting at zero; `members` lists their
names in that order. HW aliases are described by their underlying type.

## Validation

Result types are more restricted than parameter types. Supported results are
two-state scalar/native integers (widths 1/8/16/32/64), one-bit logic, real,
shortreal, string, and chandle. Wide vectors and aggregates must be conveyed as
parameters, for example using `out`, not as function results.

Unknown types, zero-width integers, unsupported nested members, nonstandard
floating-point formats, unpacked unions, queues, events, and invalid results
cause failure. The output stream is unchanged on failure, even if earlier
declarations were valid. This validation does not promise that existing
SimToSV/ExportVerilog implementations support every described standard type.
