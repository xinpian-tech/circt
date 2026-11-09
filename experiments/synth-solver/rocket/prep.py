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
- memories (seq.firmem) of depth <= MEM_DEPTH become registers with free
  initial contents (read latency, enables, write masks as in seq.firmem;
  read-under-write returns the old value); deeper ones are abstracted: read
  data is a free input and writes are dropped (over-approximation: proofs
  hold, counterexamples may be spurious);
- the EICG clock gate passes its clock through (gating is ignored);
- plusarg_reader outputs its DEFAULT parameter (its synthesis semantics);
  other external instances: outputs are free inputs;
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
from circt.ir import (Attribute, Block, Context, IntegerAttr, InsertionPoint, IntegerType,
                      Location, Module, Operation, StringAttr)
from circt.passmanager import PassManager


MEM_DEPTH = 64
SEG = None  # compreg operandSegmentSizes, set in main()


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
        lowered = self.memories(ops, first, clock, term, dead)
        for op in ops:
            name = op.operation.name
            if op.operation in lowered:
                continue
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
            try:
                op = op.operation
                op.name
            except RuntimeError:
                continue  # already erased by an earlier rewrite (seen with -O=debug)
            op.erase()
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

    def segments(self, op):
        """Operands of a firmem port with None for absent optional ones
        (segment sizes are a property, not visible here, so presence is read
        from the printed op)."""
        text = str(op).split(":")[0]
        has_en, has_mask = " enable " in text, " mask " in text
        ops = list(op.operands)
        if op.name == "seq.firmem.read_port":
            return ops[:3] + [ops[3] if has_en else None]
        head, rest = ops[:3], ops[3:]
        en = rest.pop(0) if has_en else None
        mask = rest.pop() if has_mask else None
        return head + [en] + rest + [mask]

    def memories(self, ops, first, clock, term, dead):
        """Lower seq.firmem with depth <= MEM_DEPTH to registers; larger ones
        stay for the abstraction (free read data). Returns lowered ops."""
        mems = {}
        for op in ops:
            if op.operation.name == "seq.firmem":
                mems[op.operation] = []
        for op in ops:
            n = op.operation.name
            if n.startswith("seq.firmem.") and op.operands[0].owner.operation in mems:
                mems[op.operands[0].owner.operation].append(op.operation)
        lowered = set()
        for mem, ports in mems.items():
            m = re.search(r"<(\d+) x (\d+)(?:, mask (\d+))?>",
                          str(mem.results[0].type))
            depth, width = int(m.group(1)), int(m.group(2))
            if depth > MEM_DEPTH:
                continue
            rlat = IntegerAttr(mem.attributes["readLatency"]).value
            ty = i(width)
            with InsertionPoint(term):
                regs, vals = [], []
                for _ in range(depth):
                    # placeholder input, fixed below; free initial contents
                    r = create("seq.compreg", [self.const(0, width), clock],
                               [ty], {"operandSegmentSizes": SEG})[0]
                    regs.append(r)
                    vals.append(create("comb.mux",
                                       [first, r, self.free(ty)], [ty])[0])
                nxt = list(vals)
                for p in ports:
                    if p.name == "seq.firmem.read_port":
                        _, addr, _, en = self.segments(p)
                        self.read(p.results[0], vals, addr, en, rlat, clock)
                    elif p.name == "seq.firmem.write_port":
                        _, addr, _, en, data, mask = self.segments(p)
                        nxt = self.write(nxt, addr, en, data, mask, None)
                    elif p.name == "seq.firmem.read_write_port":
                        _, addr, _, en, wdata, mode, mask = self.segments(p)
                        self.read(p.results[0], vals, addr, en, rlat, clock)
                        nxt = self.write(nxt, addr, en, wdata, mask, mode)
                for r, v in zip(regs, nxt):
                    r.owner.operation.operands[0] = v
            dead.append(mem)  # erased after its ports (dead is reversed)
            lowered.add(mem)
            for p in ports:
                dead.append(p)
                lowered.add(p)
        return lowered

    def select(self, vals, addr):
        aw = IntegerType(addr.type).width
        out = vals[-1]
        for k in range(len(vals) - 2, -1, -1):
            hit = create("comb.icmp", [addr, self.const(k, aw)], [i(1)],
                         {"predicate": Attribute.parse("0 : i64")})[0]
            out = create("comb.mux", [hit, vals[k], out], [vals[k].type])[0]
        return out

    def read(self, res, vals, addr, en, rlat, clock):
        ty = res.type
        data = self.select(vals, addr)
        for _ in range(rlat):
            # registered read: holds its value when not enabled
            r = create("seq.compreg", [data, clock], [ty],
                       {"operandSegmentSizes": SEG})[0]
            if en is not None:
                r.owner.operation.operands[0] = create(
                    "comb.mux", [en, data, r], [ty])[0]
            data = r
        res.replace_all_uses_with(data)

    def write(self, nxt, addr, en, data, mask, mode):
        ty = data.type
        width = IntegerType(ty).width
        aw = IntegerType(addr.type).width
        we = en if en is not None else self.const(1, 1)
        if mode is not None:
            we = create("comb.and", [we, mode], [i(1)])[0]
        full = None
        if mask is not None:
            mb = IntegerType(mask.type).width
            g = width // mb
            parts = []
            for j in range(mb - 1, -1, -1):
                bit = create("comb.extract", [mask], [i(1)],
                             {"lowBit": Attribute.parse(f"{j} : i32")})[0]
                parts.append(create("comb.replicate", [bit], [i(g)])[0]
                             if g > 1 else bit)
            full = parts[0] if len(parts) == 1 else create(
                "comb.concat", parts, [ty])[0]
        out = []
        for k, old in enumerate(nxt):
            hit = create("comb.icmp", [addr, self.const(k, aw)], [i(1)],
                         {"predicate": Attribute.parse("0 : i64")})[0]
            w = create("comb.and", [we, hit], [i(1)])[0]
            new = data
            if full is not None:
                keep = self.bnot(full)
                new = create("comb.or", [
                    create("comb.and", [data, full], [ty])[0],
                    create("comb.and", [old, keep], [ty])[0]], [ty])[0]
            out.append(create("comb.mux", [w, new, old], [ty])[0])
        return out

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
        elif target == "plusarg_reader":
            # synthesis semantics of plusarg_reader.v: out = DEFAULT
            d = int(re.search(r"DEFAULT: \w+ = (\d+)", str(op)).group(1))
            with InsertionPoint(op):
                op.results[0].replace_all_uses_with(
                    self.const(d, IntegerType(op.results[0].type).width))
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
        global SEG
        SEG = Attribute.parse("array<i32: 1, 1, 0, 0, 0>")
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
