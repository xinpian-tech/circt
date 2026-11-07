# ImportVerilog issues (for upstream llvm/circt)

Found while importing the CV32E40X core with its SVA. Each issue has a
minimal reproducer in this directory. All were checked against upstream main
`fcd8507a3` (2026-11-06) with `circt-verilog FILE.sv`. "Branch" refers to
xinpian-tech/circt `exp/synth-for-solver`, which has fixes for some of them
(commits listed; they can be turned into PRs).

| # | File | Kind | Upstream | Branch |
|---|------|------|----------|--------|
| 1 | 01-nonoverlap-implication.sv | wrong semantics, silent | wrong IR | fixed (6d9159ae0) |
| 2 | 02-named-property-disable-iff.sv | rejected | error | fixed (6d9159ae0) |
| 3 | 03-covergroup.sv, 03b-covergroup-instance.sv | rejected | error | declaration fixed (6d9159ae0); instance open |
| 4 | 04-hier-ref-sibling.sv, 04b-hier-ref-sibling-bind.sv | wrong semantics, silent | free input | fixed (5854bf3d4, f58e05fb7) |
| 5 | 05-stable-unpacked-array.sv | rejected | error | fixed (dc33b7cd0; `$stable`/`$changed` only) |
| 6 | 06-param-array-of-structs-index.sv | invalid IR | verifier error | fixed (dc33b7cd0; verifier unchanged) |
| 7 | 07-hier-ref-interface-member.sv | rejected | error | open |
| 8 | 08-assertion-labels.sv | information lost | no labels | fixed (uncommitted at time of writing) |

## Upstream status (searched 2026-11-07)

Searched all llvm/circt issues and PRs mentioning ImportVerilog,
circt-verilog, slang or Moore (782) for each topic.

| # | Existing upstream report | Action |
|---|--------------------------|--------|
| 1 | none (#10191 and #6930 use `\|=>` but do not notice the encoding) | new issue |
| 2 | related: #10191 (open) gets the same `unsupported expression: DisableIff` from `disable iff` under a default clocking block; #9875 (merged) handles `@(clk) disable iff` written inline. Named properties are not covered | new issue, mention #10191 |
| 3 | none | new issue |
| 4 | none; merged hierarchical-name fixes (#10255 deduped instances, #10341, #10773 subroutines) do not cover references into sibling subtrees | new issue |
| 5 | #10829 (merged) added packed aggregates to sampled value functions; unpacked arrays still fail | new issue, mention #10829 |
| 6 | none; #7918 (open) is a different wrong `hw.array_slice` (extract of an array of arrays) | new issue, mention #7918 |
| 7 | **#11122 (open), fix in PR #11130 (open)**: "Resolve hierarchical outputs from expanded interfaces", same error with a parent reading `second.command.valid` | do not file; add this reproducer (sibling port connection) as a comment on #11122 and check #11130 against it |
| 8 | none; #11065 (merged) and #10796 (closed) are about where labeled assertions are emitted, not the `label` attribute | new issue |

Issues 1 and 4 are the most serious: the import succeeds but the result
means something else, so a formal tool proves or refutes the wrong property.

---

## 1. `a |=> b` is imported as `a |-> b` (one cycle late)

```systemverilog
module top(input logic clk, input logic a, input logic b);
  assert property (@(posedge clk) a |=> b);
endmodule
```

Upstream output:

```mlir
%0 = ltl.delay %a, 1, 0 : i1
%1 = ltl.concat %0, %true : !ltl.sequence, i1
%2 = ltl.implication %1, %b : !ltl.sequence, i1
verif.clocked_assert %2, posedge %clk : !ltl.property
```

`ltl.delay %a, 1, 0` starts in cycle t and checks `a` in t+1, where it ends.
`ltl.concat` lets the next sequence start in the cycle where the previous
one ends (LTL dialect docs: "no implicit cycle of delay"), so `%true` is also
in t+1, the antecedent ends in t+1, and `ltl.implication` (overlapping)
checks `b` in t+1, the same cycle as `a`. That is `a |-> b`, shifted by one
cycle. Example: with `always_ff @(posedge clk) y <= go;`, `go |=> y` holds,
but the imported property (`go` implies `y` in the same cycle) fails in
every cycle where `go` rises.

Expected: `concat(a, delay(true, 1, 0)) |-> b`, i.e. `a ##1 1 |-> b`:

```mlir
%0 = ltl.delay %true, 1, 0 : i1
%1 = ltl.concat %a, %0 : i1, !ltl.sequence
%2 = ltl.implication %1, %b : !ltl.sequence, i1
```

The same applies to `#=#` (non-overlapping followed-by). Code:
`AssertionExpr.cpp`, the `BinaryAssertionOperator::NonOverlappedImplication`
and `NonOverlappedFollowedBy` cases.

## 2. Clocking or `disable iff` inside a named property is rejected

```systemverilog
module top(input logic clk, input logic rst, input logic a, input logic b);
  property p;
    @(posedge clk) disable iff (rst) a |-> b;
  endproperty
  assert property (p);
endmodule
```

Upstream:

```
02-named-property-disable-iff.sv:6:20: error: unsupported expression: DisableIff
  assert property (p);
                   ^
```

The `disable iff` / clocking detection in `Statements.cpp`
(`visit(ConcurrentAssertionStatement)`) only looks at the top-level
expression of the assertion; here it is an `AssertionInstanceExpression` of
`p`, whose body holds the clocking and `disable iff`. This form is common
(e.g. throughout the CV32E40X SVA). Expected: the same result as writing the
property inline. Fix: look through (non-recursive) property instances when
looking for the clocking and `disable iff`.

## 3. Covergroups in modules are rejected

```systemverilog
module top(input logic clk, input logic [1:0] x);
  covergroup cg @(posedge clk);
    coverpoint x;
  endgroup
endmodule
```

Upstream:

```
03-covergroup.sv:3:14: error: unsupported module member: CovergroupType
```

With an instance (`03b-covergroup-instance.sv`, `cg cg_i = new();`):

```
03b-covergroup-instance.sv:6:3: error: unsupported type: CovergroupType
```

Functional coverage has no meaning for synthesis or formal tools, so
ignoring covergroup declarations (and their instances) with at most a
warning would let such designs import. The branch skips declarations only;
instances are still rejected.

## 4. Hierarchical reference into a sibling subtree becomes an unconnected input

```systemverilog
module core(input logic clk, input logic d);
  logic state;
  always_ff @(posedge clk) state <= d;
endmodule
module checker_m(input logic clk, input logic s);
  assert property (@(posedge clk) s == s);
endmodule
module ctrl(input logic clk);
  checker_m chk(.clk(clk), .s(top.u_core.state));
endmodule
module top(input logic clk, input logic d);
  core u_core(.clk(clk), .d(d));
  ctrl u_ctrl(.clk(clk));
endmodule
```

Upstream (no error or warning):

```mlir
hw.module private @core(in %clk : !seq.clock, in %d : i1) { ... }
hw.module private @ctrl(in %clk : i1, in %u_core.state : !llhd.ref<i1>) { ... }
hw.module @top(in %clk : !seq.clock, in %d : i1, in %u_core.state : !llhd.ref<i1>) {
  hw.instance "u_core" @core(clk: %clk: !seq.clock, d: %d: i1) -> ()
  hw.instance "u_ctrl" @ctrl(clk: %0: i1, u_core.state: %u_core.state: !llhd.ref<i1>) -> ()
```

The reference is passed down from `top` as an input, but nothing exports
`state` from `u_core`: `top` gets a new input port `u_core.state`, so the
checker reads an unconstrained value instead of the register. A reference
from a direct child (`checker_m chk(.s(u_core.state))` in `top`) works. The
common case is SVA attached with `bind` (`04b-hier-ref-sibling-bind.sv`,
`bind ctrl checker_m chk(.clk(clk), .s(top.u_core.state));`), which gives
the same result.

Expected: `core` gets an output `state` (as for references from the
parent), and `top` connects it to `u_ctrl`. The same happens for
references to input ports (`top.u_core.d`).

The branch fix (`HierarchicalNames.cpp`, `Structure.cpp`): the input chain
stops at the first module that also contains the referenced symbol (the
common ancestor), output ports are added from the symbol's module up to it,
and if the referencing instance is converted before the providing one, a
placeholder is used and resolved later. Values that are not references
(e.g. input ports) are passed through a wire. Bind-elaborated instances are
matched by name and definition, and placeholders that stay unresolved are
reported as errors instead of leaking into the output.

## 5. `$stable` of an unpacked array is rejected

```systemverilog
module top(input logic clk, input logic [7:0] a [2]);
  assert property (@(posedge clk) $stable(a));
endmodule
```

Upstream:

```
05-stable-unpacked-array.sv:3:35: error: expected integer argument for `$stable`
```

slang accepts this (sampled value functions, IEEE 1800-2017 16.9.3); the
CV32E40X debug-trigger SVA uses `$stable` on arrays of trigger registers.
Expected: compare the whole array (e.g. element-wise).

Branch fix (`AssertionExpr.cpp`): for `$stable` and `$changed`, unpacked
arrays and structs are concatenated (recursively, element by element) into
one integer, which is then compared with its `ltl.past` value. `$past`,
`$rose` and `$fell` of unpacked values are still rejected.

## 6. Dynamic index into a constant array of packed structs gives invalid IR

```systemverilog
typedef struct packed {
  logic [7:0] lo;
  logic b;
} cfg_t;
module top(input logic idx, output cfg_t o);
  localparam cfg_t CFG [2] = '{'{lo: 8'h1, b: 1'b0}, '{lo: 8'h2, b: 1'b1}};
  assign o = CFG[idx];
endmodule
```

Upstream:

```
06-param-array-of-structs-index.sv:9:14: error: 'hw.array_slice' op result #0 must be an ArrayType, but got '!hw.struct<lo: i8, b: i1>'
note: see current operation: %5 = "hw.array_slice"(%3, %4) : (!hw.array<2xi9>, i1) -> !hw.struct<lo: i8, b: i1>
```

Already in the Moore IR (`--ir-moore`), the constant array is built with
integer elements while the index result is the struct:

```mlir
%3 = moore.array_create %2, %1 : !moore.l9, !moore.l9 -> uarray<2 x l9>
%5 = moore.dyn_extract %3 from %4 : uarray<2 x l9>, l1 -> struct<{lo: l8, b: l1}>
```

So two problems: the constant of an unpacked array of packed structs is
materialized with the packed integer type as element type, and the
`moore.dyn_extract` verifier accepts an element type that differs from the
result type (MooreToCore then emits the invalid `hw.array_slice`). A
variable array of the same type (`cfg_t arr [2]`) imports correctly. Found
with a `parameter pma_cfg_t PMA_CFG[...]` indexed in a loop (CV32E40X MPU).

Branch fix (`Expressions.cpp`, `materializeFixedSizeUnpackedArrayType`):
each element is materialized with `materializeConstant` and the array's
element type (packed structs, packed arrays, strings, nested unpacked
arrays), instead of an integer of the element's width. The
`moore.dyn_extract` verifier is unchanged.

## 7. Hierarchical reference to an interface member in a sibling subtree

```systemverilog
interface bus_if;
  logic valid;
endinterface
module core(input logic clk, input logic d);
  bus_if bus();
  always_ff @(posedge clk) bus.valid <= d;
endmodule
module checker_m(input logic clk, input logic v);
  assert property (@(posedge clk) v == v);
endmodule
module top(input logic clk, input logic d);
  core u_core(.clk(clk), .d(d));
  checker_m chk(.clk(clk), .v(u_core.bus.valid));
endmodule
```

Upstream (and branch):

```
07-hier-ref-interface-member.sv:7:8: error: unable to resolve hierarchical output `bus.valid` in module `core`
```

Interface instances are flattened into the instantiating module, so the
member is a signal of `core`; the hierarchical-name analysis does not find
it there (probably it looks for an output of the interface instance).
Expected: export the flattened member signal from `core`.

## 8. Assertion labels are dropped

```systemverilog
module top(input logic clk, input logic a, input logic b);
  a_foo: assert property (@(posedge clk) a |-> b);
  m_baz: assume property (@(posedge clk) a);
  always_comb begin
    a_bar: assert (a);
  end
endmodule
```

Upstream:

```mlir
verif.clocked_assert %0, posedge %clk : !ltl.property
verif.clocked_assume %a, posedge %clk : i1
verif.assert %a label "" : i1
```

The verif and moore assertion ops have a `label` attribute, but
`Statements.cpp` always passes an empty one. Expected `label "a_foo"`,
`label "m_baz"`, `label "a_bar"`: labels are how verification flows name
and select properties (one model per assertion, reports). The label is
`StatementSyntax::label` of the statement's syntax.
