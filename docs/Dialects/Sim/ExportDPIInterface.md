# Sim DPI Interface JSON

`circt::sim::exportDPIInterface` describes imported `sim.func.dpi`
declarations as they will appear in SV DPI imports. The C API
`mlirExportDPIInterface` delivers the same JSON through a callback. Neither
interface changes the IR or performs SV lowering.

The JSON format is specific to CIRCT. Its type and direction rules follow
[IEEE 1800-2023](https://standards.ieee.org/ieee/1800/7743/), Clause 35 and
Annex H; IEEE does not define a DPI JSON format.

## Functions and Arguments

The root object contains `dpi_functions`, an array in declaration order.
Each function has `function` (the `verilogName`, or the symbol name when absent)
and `arguments`, in signature order. An empty argument array is valid.
Function names must be valid C/SV identifiers and unique within the export.
Argument names describe the source arguments; consumers may sanitize their
local C/C++ parameter names without changing the external function name.

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
| Array | `type: "array"`, `packed`, `size`, `element` |
| Structure | `type: "struct"`, `packed`, `members` |
| Packed union | `type: "union"`, `packed: true`, `members` |
| Enumeration | `type: "enum"`, `width`, `signed`, `members` |

Integer widths are not limited to 64 bits. The integer keyword and effective
signedness are resolved using the shared SV DPI type mapping, not the source
MLIR integer's signedness. This preserves the existing Verilog output:

| Source type | Effective SV type | JSON signedness |
| --- | --- | --- |
| `i32`, `si32`, or `ui32` | `int` | `true` |
| `i7`, `si7`, or `ui7` | `bit [6:0]` | `false` |
| `!hw.array<2xi32>` | `bit [1:0][31:0]` | Element: `false` |
| `!hw.uarray<2xi32>` | `int value[0:1]` | Element: `true` |
| Alias to a typedef of `i32` | `logic [31:0]` via the typedef | `false` |

Without packed dimensions, widths 8/16/32/64 use `byte`, `shortint`, `int`, and
`longint`, which are signed. Width 1 uses `bit`; other widths and packed array
elements use unsigned bit vectors. Existing SV export does not preserve
explicit MLIR integer signedness in these declarations. JSON describes this
behavior rather than promising a different interface.

Aliases must resolve to matching HW typedef declarations. Their descriptions
follow the actual typedef. Typedefs of builtin integers use four-state `logic`
vectors rather than the native two-state integer atom convention. Symbolic
dimensions and unresolved or mismatched aliases cannot produce a concrete
interface.

An array's `size` is its element count, or `null` for an open dimension.
`element` is another type description. Nested array descriptions preserve the
dimensions; fixed dimensions use the canonical ranges implied by the source IR.
Packed elements must be integral. Arrays accept HW fixed arrays and SV open
unpacked arrays.

Structure and union `members` are ordered type descriptions with an additional
`name`. HW union members also include their bit `offset`, preserving the HW
layout (including any padding required for SV emission). Packed structure
members follow SV declaration order, with the first member at the most
significant end. Unpacked structures retain member order without assuming C
padding. HW enums have sequential values starting at zero; `members` lists their
names in that order. HW aliases are described by their effective typedef type.

## Validation

Result types are more restricted than parameter types. Supported results are
two-state scalar/native integers (widths 1/8/16/32/64) and one-bit logic typedefs.
Wide vectors and aggregates must be conveyed as parameters, for example using
`out`, not as function results.

Only types supported by the current SV DPI mapping are accepted. In particular,
floats, strings, LLVM pointers, and Moore types are rejected even when a
corresponding type exists in the DPI standard: the existing SV printer cannot
emit these source types. Unknown types, zero-width integers, unsupported nested
members, unpacked unions, queues, events, and invalid results also cause
failure. The output stream is unchanged on failure, even if earlier declarations
were valid. Exporting JSON does not add support to SimToSV or ExportVerilog.
