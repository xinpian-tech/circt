#!/usr/bin/env python3
"""Turn firtool `--lower-to-core --ir-hw` output into a flat safety model.

Uses the CIRCT Python bindings. The result is one hw.module with a clock,
free inputs and one output per Chisel assert (`bad_K = en & ~pred`), only
compreg registers, ready for the synthesis configurations and AIGER export.

Model of the environment (over-approximations are marked):
- reset: the top `reset` port is forced high in cycle 0 (a `first` register
  starts at 0 and is then 1) and free afterwards;
- registers with a (sync or async) reset: compreg of mux(reset, rv, next);
  registers without reset start at a free value: s = first ? L : free;
- memories (seq.firmem) left by firtool: read data is a free input, writes
  are dropped (over-approximation: proofs hold, counterexamples may be
  spurious);
- the EICG clock gate passes its clock through (gating is ignored);
- other external instances (simulation helpers): outputs are free inputs;
- sim.* (printf) ops are dropped; a clock used as data (seq.from_clock)
  is a free input.

Free inputs are created as `verif.symbolic_value` and turned into module
ports in the printed text (the only text edit).

    prep.py IN.mlir OUT.mlir [--top TestHarness] [--asserts LIST]

`--asserts` keeps only the asserts with the given indices (comma separated)
as outputs; by default all are kept.
"""

import argparse
import re

import circt
from circt.ir import (Attribute, Block, Context, InsertionPoint, IntegerType,
                      Location, Module, Operation, StringAttr)
from circt.passmanager import PassManager


def i(w):
    return IntegerType.get_signless(w)


def create(name, operands, result_types, attrs=None):
    return Operation.create(name, results=result_types, operands=operands,
                            attributes=attrs or {}).results


class Prep:

    def __init__(self, top):
        self.top = top
        self.free_count = 0

    def free(self, ty):
        self.free_count += 1
        return create("verif.symbolic_value", [], [ty])[0]

    def const(self, v, w):
        return create("hw.constant", [], [i(w)],
                      {"value": Attribute.parse(f"{v} : i{w}")})[0]

    def bnot(self, x):
        return create("comb.xor", [x, self.const(-1 if x.type != i(1) else 1,
                                                  IntegerType(x.type).width)],
                      [x.type])[0]

    def run(self, mod):
        body = self.top.regions[0].blocks[0]
        ops = list(body.operations)
        clock = None
        reset = None
        for a, n in zip(body.arguments, self.port_names()):
            if n == "reset":
                reset = a
            if n == "clock":
                clock = a
        term = ops[-1]
        asserts = []
        with InsertionPoint.at_block_begin(body):
            # first: 0 in cycle 0, then 1; reset_eff = reset | ~first.
            one = self.const(1, 1)
            first = create("seq.compreg", [one, clock], [i(1)], {
                "operandSegmentSizes":
                Attribute.parse("array<i32: 1, 1, 0, 0, 0>")
            })[0]
            reset_eff = None
            if reset is not None:
                reset_eff = create("comb.or", [reset, self.bnot(first)],
                                   [i(1)])[0]
        if reset is not None:
            skip = reset_eff.owner
            for use in list(reset.uses):
                owner = use.owner
                if owner.operation == skip.operation if hasattr(
                        skip, "operation") else owner == skip:
                    continue
                owner.operands[use.operand_number] = reset_eff
        dead = []  # erased at the end, users first
        for op in ops:
            name = op.operation.name
            if name.startswith("sim."):
                dead.append(op)
            elif name == "verif.clocked_assert":
                asserts.append(op)
            elif name == "seq.firreg":
                self.firreg(op.operation, first)
            elif name in ("seq.firmem.read_port",
                          "seq.firmem.read_write_port"):
                with InsertionPoint(op):
                    f = self.free(op.results[0].type)
                op.results[0].replace_all_uses_with(f)
                dead.append(op)
            elif name == "hw.wire":
                op.results[0].replace_all_uses_with(op.operands[0])
                dead.append(op)
            elif name == "seq.from_clock":
                # a clock used as data: free (single-clock AIGER semantics)
                with InsertionPoint(op):
                    f = self.free(i(1))
                op.results[0].replace_all_uses_with(f)
                dead.append(op)
            elif name in ("seq.firmem.write_port", "seq.firmem"):
                dead.append(op)
            elif name == "hw.instance":
                self.instance(op.operation)
        for op in reversed(dead):
            op.operation.erase()
        bads = []
        with InsertionPoint(term):
            for a in asserts:
                prop, en = self.assert_operands(a.operation)
                np = self.bnot(prop)
                bads.append(np if en is None else create(
                    "comb.and", [en, np], [i(1)])[0])
        labels = [self.label(a.operation) for a in asserts]
        for a in asserts:
            a.operation.erase()
        return bads, labels

    def port_names(self):
        mt = self.top.attributes["module_type"]
        return re.findall(r"input (\w+)", str(mt))

    def assert_operands(self, op):
        # operands: property, clock, [enable]
        ops = list(op.operands)
        if len(ops) == 3:
            return ops[0], ops[2]
        return ops[0], None

    def label(self, op):
        try:
            return StringAttr(op.attributes["label"]).value
        except KeyError:
            return ""

    def firreg(self, op, first):
        nxt, clk = op.operands[0], op.operands[1]
        res = op.results[0]
        with InsertionPoint(op):
            if len(op.operands) >= 4:
                rst, rv = op.operands[2], op.operands[3]
                d = create("comb.mux", [rst, rv, nxt], [res.type])[0]
                r = create("seq.compreg", [d, clk], [res.type], {
                    "operandSegmentSizes":
                    Attribute.parse("array<i32: 1, 1, 0, 0, 0>")
                })[0]
                new = r
            else:
                r = create("seq.compreg", [nxt, clk], [res.type], {
                    "operandSegmentSizes":
                    Attribute.parse("array<i32: 1, 1, 0, 0, 0>")
                })[0]
                new = create("comb.mux", [first, r, self.free(res.type)],
                             [res.type])[0]
        res.replace_all_uses_with(new)
        op.erase()

    def instance(self, op):
        target = str(op.attributes["moduleName"]).strip('"@')
        if target == "EICG_wrapper":
            op.results[0].replace_all_uses_with(op.operands[0])
        else:
            with InsertionPoint(op):
                for r in op.results:
                    r.replace_all_uses_with(self.free(r.type))
        op.erase()


def coi(top):
    """Erase every op outside the cone of influence of the output (through
    register inputs). Only removes ops; nothing is simplified."""
    body = top.regions[0].blocks[0]
    ops = [op.operation for op in body.operations]
    live = set()
    stack = [ops[-1]]
    while stack:
        op = stack.pop()
        if op in live:
            continue
        live.add(op)
        for v in op.operands:
            if not isinstance(v.owner, Block):
                stack.append(v.owner.operation)
    dead = [op for op in ops if op not in live]
    # Cut dead register loops (the register input becomes a dead
    # placeholder), then erase users before definitions.
    for op in list(dead):
        if op.name == "seq.compreg":
            with InsertionPoint(op):
                ph = create("verif.symbolic_value", [], [op.results[0].type])
            op.operands[0] = ph[0]
            dead.append(ph[0].owner.operation)
    count = {op: sum(1 for r in op.results for u in r.uses) for op in dead}
    ready = [op for op in dead if count[op] == 0]
    while ready:
        op = ready.pop()
        defs = [v.owner.operation for v in op.operands
                if not isinstance(v.owner, Block)]
        op.erase()
        for d in defs:
            if d in count and d is not op:
                count[d] -= 1
                if count[d] == 0:
                    ready.append(d)


def symbolic_to_ports(text, top):
    """verif.symbolic_value lines become input ports of the top module."""
    ports = []
    out = []
    for l in text.splitlines():
        m = re.match(r"\s*(%[\w.$-]+) = verif\.symbolic_value : (\S+)", l)
        if m:
            ports.append((m.group(1), m.group(2)))
            continue
        out.append(l)
    text = "\n".join(out) + "\n"
    # Give the free values port-friendly names.
    rename = {old: f"%free{k}" for k, (old, _) in enumerate(ports)}
    if rename:
        pat = re.compile("(?:" + "|".join(re.escape(o) for o in
                                  sorted(rename, key=len, reverse=True)) +
                         r")(?![\w.$-])")
        text = pat.sub(lambda m: rename[m.group(0)], text)
    ports = [(rename[o], t) for o, t in ports]
    head = re.compile(r"(hw\.module @" + top + r"\()")
    extra = "".join(f"in {n} : {t}, " for n, t in ports)
    return head.sub(lambda m: m.group(1) + extra, text, count=1), len(ports)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--top", default="TestHarness")
    ap.add_argument("--asserts", default=None)
    ap.add_argument("--labels", default=None,
                    help="write assert index and label to this file")
    args = ap.parse_args()
    with Context() as ctx, Location.unknown():
        circt.register_dialects(ctx)
        ctx.allow_unregistered_dialects = False
        m = Module.parse(open(args.inp).read())
        PassManager.parse(
            "builtin.module(hw-flatten-modules{hw-inline-with-state=true})"
            ).run(m.operation)
        top = next(op for op in m.body.operations
                   if op.operation.name == "hw.module" and
                   StringAttr(op.attributes["sym_name"]).value == args.top)
        p = Prep(top)
        bads, labels = p.run(m)
        keep = range(len(bads)) if args.asserts is None else [
            int(x) for x in args.asserts.split(",")]
        term = list(top.regions[0].blocks[0].operations)[-1]
        with InsertionPoint(term):
            acc = None
            for k in keep:
                acc = bads[k] if acc is None else create(
                    "comb.or", [acc, bads[k]], [i(1)])[0]
            if acc is None:
                acc = p.const(0, 1)
        if args.labels:
            with open(args.labels, "w") as f:
                for k, l in enumerate(labels):
                    f.write(f"{k}\t{l}\n")
        # Replace the original outputs by the single bad output: build the
        # module text with the new output instead of editing port lists.
        term.operation.erase()
        with InsertionPoint(top.regions[0].blocks[0]):
            create("hw.output", [acc], [])
        coi(top)

        # Keep only the top module (drops hierpaths, OM, emit, verbatims).
        for op in list(m.body.operations):
            if op.operation != top.operation:
                op.operation.erase()
        text = str(m)
    # Output ports: the original top outputs are replaced by `bad`.
    text = re.sub(r"(hw\.module @" + args.top + r"\(.*?)(, out [^)]*)?\)",
                  lambda mm: mm.group(1) + ", out bad : i1)", text, count=1)
    text, nfree = symbolic_to_ports(text, args.top)
    open(args.out, "w").write(text)
    print(f"asserts {len(bads)} kept {len(list(keep))} free-inputs {nfree}")


if __name__ == "__main__":
    main()
