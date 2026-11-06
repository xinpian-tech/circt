#!/usr/bin/env python3
"""Prepare circt-verilog output of one module for circt-synth and AIGER export.

AIGER export (ExportAIGER.cpp) only takes seq.compreg and ignores its reset,
reset value and initial value: every latch starts at 0 and has no reset. This
script makes the reset explicit so the exported AIG keeps the semantics:

- register with reset (sync, or async treated as sync) and constant reset
  value C: the latch stores r ^ C, so its AIGER initial value 0 means r = C
  (the design starts in its reset state); next = mux(rst, C, d) ^ C;
- register with reset and non-constant reset value: compreg of
  mux(rst, rv, d), and r = first ? latch : rv in cycle 0;
- aggregate (struct/array) registers are lowered on their bits;
- register without reset: --noreset free (default) gives it a free initial
  value (r = first ? latch : free), --noreset zero keeps AIGER's 0; a preset
  (initial value) is used if present.

`first` is a 1-bit register that is 0 in cycle 0 and 1 afterwards. The reset
input stays a free input after cycle 0. Free values become input ports
(`free*`) of the module.

Piecewise-assigned LLHD signals are resolved (see split_signals).

Before that, the script runs the passes that remove what circt-synth does
not take (with circt-opt from PATH): module flattening, LLHD signal promotion, bitcasts and aggregates.

Assertions (verif.clocked_assert from SVA, after lower-ltl-to-core) become
one output `bad` (see Props); --assume turns some into assumptions.

    lower_regs.py IN.mlir OUT.mlir [--top NAME] [--noreset free|zero]
        [--assert L1,L2] [--assume L3,L4] [--list]
"""

import argparse
import os
import re
import subprocess
import sys

import circt
from circt.dialects import hw
from circt.ir import (Attribute, Context, InsertionPoint, IntegerAttr,
                      IntegerType, Location, Module, Operation, StringAttr,
                      Type)

PRE = ("builtin.module(hw-flatten-modules,"
       "hw.module(llhd-hoist-signals,llhd-sig2reg,llhd-combine-drives,"
       "canonicalize{SUBST},lower-ltl-to-core,lower-seq-shiftreg,"
       "lower-seq-compreg-ce),cse,hw-flatten-io,"
       "hw.module(hw-aggregate-to-comb,canonicalize),cse)")
# --substitute-antecedent: simplify each assertion's consequent under its
# antecedent (verif-substitute-antecedent) before the assertions are lowered.
SUBST = ",verif-substitute-antecedent,canonicalize"
# Bitcasts (also those of aggregate registers below) are converted last:
# hw-aggregate-to-comb turns struct_create into hw.bitcast, and canonicalize
# folds the struct_create/struct_explode pairs left by hw-convert-bitcasts.
POST = ("builtin.module(canonicalize,cse,hw-convert-bitcasts,canonicalize,"
        "hw-convert-bitcasts,canonicalize,cse)")
# compreg operands: input, clk, reset, resetValue, initialValue
SEG = "array<i32: 1, 1, 0, 0, 0>"


def i(w):
    return IntegerType.get_signless(w)


def create(name, operands, result_types, attrs=None):
    return Operation.create(name, results=result_types, operands=operands,
                            attributes=attrs or {}).results


def const(v, w):
    return create("hw.constant", [], [i(w)],
                  {"value": IntegerAttr.get(i(w), v)})[0]


def bitwidth(ty):
    if isinstance(ty, IntegerType):
        return IntegerType(ty).width
    if hw.ArrayType.isinstance(ty):
        a = hw.ArrayType(ty)
        return a.size * bitwidth(a.element_type)
    if hw.StructType.isinstance(ty):
        return sum(bitwidth(t) for _, t in hw.StructType(ty).get_fields())
    raise SystemExit(f"unsupported register type {ty}")


def bitcast(v, ty):
    return create("hw.bitcast", [v], [ty])[0]


def const_value(v):
    """Integer value of v if it is defined by hw.constant, else None."""
    try:
        op = v.owner.operation
    except AttributeError:
        return None  # block argument
    if op.name != "hw.constant":
        return None
    return IntegerAttr(op.attributes["value"]).value


def compreg(d, clk, name=None):
    attrs = {"operandSegmentSizes": Attribute.parse(SEG)}
    if name is not None:
        attrs["name"] = name
    return create("seq.compreg", [d, clk], [d.type], attrs)[0]


class Lower:

    def __init__(self, top, noreset):
        self.top = top
        self.noreset = noreset
        self.first = {}  # clock value -> first register
        self.stats = dict(const_reset=0, value_reset=0, async_reset=0,
                          noreset_free=0, noreset_zero=0, preset=0,
                          aggregate=0)

    def free(self, ty):
        return create("verif.symbolic_value", [], [ty])[0]

    def first_of(self, clk):
        key = clk.get_name() if hasattr(clk, "get_name") else str(clk)
        if key not in self.first:
            body = self.top.regions[0].blocks[0]
            with InsertionPoint.at_block_begin(body):
                self.first[key] = compreg(const(1, 1), clk,
                                          StringAttr.get("first"))
        return self.first[key]

    def run(self):
        body = self.top.regions[0].blocks[0]
        regs = [op.operation for op in body.operations
                if op.operation.name == "seq.firreg"]
        for op in regs:
            self.firreg(op)
        return len(regs)

    def firreg(self, op):
        res = op.results[0]
        ty = res.type
        w = bitwidth(ty)
        it = i(w)
        agg = not isinstance(ty, IntegerType)
        if agg:
            self.stats["aggregate"] += 1
        # aggregate registers are lowered on their bits (hw.bitcast)
        to_int = (lambda v: bitcast(v, it)) if agg else (lambda v: v)
        d, clk = op.operands[0], op.operands[1]
        name = op.attributes["name"] if "name" in op.attributes else None
        has_reset = len(op.operands) >= 4
        if "isAsync" in op.attributes:
            self.stats["async_reset"] += 1
        with InsertionPoint(op):
            d = to_int(d)
            if has_reset:
                rst, rv = op.operands[2], to_int(op.operands[3])
                nxt = create("comb.mux", [rst, rv, d], [it])[0]
                c = None if agg else const_value(rv)
                if c is not None:
                    self.stats["const_reset"] += 1
                    if c == 0:
                        new = compreg(nxt, clk, name)
                    else:
                        k = const(c, w)
                        enc = create("comb.xor", [nxt, k], [it])[0]
                        lat = compreg(enc, clk, name)
                        new = create("comb.xor", [lat, k], [it])[0]
                else:
                    self.stats["value_reset"] += 1
                    lat = compreg(nxt, clk, name)
                    new = create("comb.mux", [self.first_of(clk), lat, rv],
                                 [it])[0]
            elif "preset" in op.attributes:
                self.stats["preset"] += 1
                c = IntegerAttr(op.attributes["preset"]).value
                k = const(c, w)
                enc = create("comb.xor", [d, k], [it])[0]
                new = create("comb.xor", [compreg(enc, clk, name), k],
                             [it])[0]
            elif self.noreset == "zero":
                self.stats["noreset_zero"] += 1
                new = compreg(d, clk, name)
            else:
                self.stats["noreset_free"] += 1
                lat = compreg(d, clk, name)
                new = create("comb.mux",
                             [self.first_of(clk), lat, self.free(it)],
                             [it])[0]
        if agg:
            with InsertionPoint(op):
                new = bitcast(new, ty)
        res.replace_all_uses_with(new)
        op.erase()


def zero(ty):
    w = bitwidth(ty)
    c = const(0, w)
    return c if isinstance(ty, IntegerType) else bitcast(c, ty)


def split_signals(top):
    """Replace LLHD signals that are driven piecewise by continuous assigns.

    llhd-sig2reg leaves a signal alone when it is assigned per bit, array
    element or struct field and also read whole, as in a generate loop that
    builds a tree in one vector (sel[i] = sel[2i+1] | sel[2i+2]): it is a
    word-level loop but not a bit-level one. If every write is a top-level
    llhd.assign to a projection with a constant index (sig.extract,
    sig.array_get, sig.struct_extract), the probe is the concatenation of
    the assigned values; canonicalize then folds extract(concat) and the
    loop goes away. One whole assign may be combined with them (an
    always_comb that writes the struct while other fields have their own
    continuous assigns): the parts not assigned piecewise come from it.
    Undriven parts read 0. Other signals are left alone.

    Works on the generic form (the Python bindings have no LLHD dialect).
    Returns (split, kept) signal counts.
    """
    body = top.regions[0].blocks[0]
    term = list(body.operations)[-1]
    split = kept = 0
    sigs = [op.operation for op in body.operations
            if op.operation.name == "llhd.sig"]
    for sig in sigs:
        res = sig.results[0]
        prbs, slots, dead, ok, whole = [], {}, [], True, None
        for use in list(res.uses):
            u = use.owner
            if u.name == "llhd.prb":
                prbs.append(u)
                continue
            if u.name == "llhd.assign" and use.operand_number == 0 and \
                    len(u.operands) == 2 and u.parent.name == "hw.module" \
                    and whole is None:
                whole = u.operands[1]
                dead.append(u)
                continue
            if u.name not in ("llhd.sig.extract", "llhd.sig.array_get",
                              "llhd.sig.struct_extract") or \
                    use.operand_number != 0:
                ok = False
                break
            if u.name == "llhd.sig.struct_extract":
                key = StringAttr(u.attributes["field"]).value
            else:
                c = const_value(u.operands[1])
                if c is None:
                    ok = False
                    break
                key = c & ((1 << bitwidth(u.operands[1].type)) - 1)
            puses = list(u.results[0].uses)
            if len(puses) != 1 or puses[0].owner.name != "llhd.assign" or \
                    puses[0].operand_number != 0 or \
                    len(puses[0].owner.operands) != 2 or \
                    puses[0].owner.parent.name != "hw.module" or key in slots:
                ok = False
                break
            slots[key] = puses[0].owner.operands[1]
            dead += [puses[0].owner, u]
        if not ok or not prbs or not slots:
            kept += 1
            continue
        ty = prbs[0].results[0].type

        def rest(lo, w):  # bits [lo, lo+w) not assigned piecewise
            if whole is None:
                return const(0, w)
            return create("comb.extract", [whole], [i(w)],
                          {"lowBit": IntegerAttr.get(i(32), lo)})[0]

        with InsertionPoint(term):
            if isinstance(ty, IntegerType):
                parts, pos, w = [], 0, IntegerType(ty).width
                for lo in sorted(slots):
                    v = slots[lo]
                    if lo < pos:
                        ok = False  # overlapping drivers
                        break
                    if lo > pos:
                        parts.append(rest(pos, lo - pos))
                    parts.append(v)
                    pos = lo + bitwidth(v.type)
                if ok and pos > w:
                    ok = False
                if ok and pos < w:
                    parts.append(rest(pos, w - pos))
                if ok:
                    val = parts[0] if len(parts) == 1 else create(
                        "comb.concat", list(reversed(parts)), [ty])[0]
            elif hw.ArrayType.isinstance(ty):
                a = hw.ArrayType(ty)
                iw = max(1, (a.size - 1).bit_length())
                el = [slots.get(k) or (zero(a.element_type) if whole is None
                                       else create("hw.array_get",
                                                   [whole, const(k, iw)],
                                                   [a.element_type])[0])
                      for k in range(a.size)]
                ok = all(k < a.size for k in slots)
                if ok:
                    val = create("hw.array_create", list(reversed(el)),
                                 [ty])[0]
            elif hw.StructType.isinstance(ty):
                fields = hw.StructType(ty).get_fields()
                ok = set(slots) <= {n for n, _ in fields}
                if ok:
                    val = create("hw.struct_create", [
                        slots.get(n) or (zero(t) if whole is None else create(
                            "hw.struct_extract", [whole], [t],
                            {"fieldIndex": IntegerAttr.get(i(32), k)})[0])
                        for k, (n, t) in enumerate(fields)], [ty])[0]
            else:
                ok = False
        if not ok:
            kept += 1  # the ops created above are dead and removed by DCE
            continue
        for p in prbs:
            p.results[0].replace_all_uses_with(val)
        for op in dead + prbs + [sig]:
            op.erase()
        split += 1
    return split, kept


def assert_label(op):
    """SVA label (`a_name :`) at the source line of an assertion."""
    return label_at(str(op.location))


def label_at(loc):
    m = re.search(r'loc\("([^"]+)":(\d+):', loc)
    if not m:
        return None
    path = m.group(1)
    if not os.path.isabs(path):  # import.sh runs circt-verilog from /
        path = os.path.normpath(os.path.join("/", path))
    try:
        lines = open(path).read().splitlines()
    except OSError:
        return f"{m.group(1)}:{m.group(2)}"
    k = int(m.group(2)) - 1
    for l in lines[max(0, k - 2):k + 1][::-1]:
        mm = re.match(r"\s*(\w+)\s*:(?!:)", l)
        if mm:
            return mm.group(1)
    return f"{m.group(1).split('/')[-1]}:{m.group(2)}"


class Props:
    """verif assertions and assumptions to safety outputs.

    A clocked assertion `P if EN` becomes the violation EN & ~P in the cycle
    where it is checked. P is an i1, or an implication whose antecedent is a
    sequence of fixed delays (a ##1 b ...), checked at the end of the
    sequence with registers for the earlier steps (EN must hold in every
    step, as for `disable iff`). These are the forms left by
    lower-ltl-to-core for the CV32E40X SVA; others are refused.

    Assumptions: once one is violated the trace no longer counts, so
    bad = violation & (all assumptions held so far, including now).
    """

    def __init__(self, top, clk):
        self.top = top
        self.clk = clk
        self.term = list(top.regions[0].blocks[0].operations)[-1]
        self.dead = []

    def past(self, v, n):
        for _ in range(n):
            v = compreg(v, self.clk)
        return v

    def bnot(self, v):
        return create("comb.xor", [v, const(1, 1)], [i(1)])[0]

    def and_(self, vs):
        vs = [v for v in vs if v is not None]
        if not vs:
            return const(1, 1)
        return vs[0] if len(vs) == 1 else create("comb.and", vs, [i(1)])[0]

    def unwrap(self, v):
        """Strip ltl.clock (single clock) and casts of i1 to properties."""
        while True:
            try:
                op = v.owner.operation
            except AttributeError:
                return v
            if op.name in ("ltl.clock", "builtin.unrealized_conversion_cast"):
                self.dead.append(op)
                v = op.operands[0]
                continue
            return v

    def sequence(self, v):
        """[(value, time)] for a sequence of fixed delays, and its length."""
        v = self.unwrap(v)
        if isinstance(v.type, IntegerType):
            return [(v, 0)], 0
        op = v.owner.operation
        self.dead.append(op)
        if op.name == "ltl.delay":
            d = IntegerAttr(op.attributes["delay"]).value
            if "length" not in op.attributes or \
                    IntegerAttr(op.attributes["length"]).value != 0:
                raise SystemExit(f"unbounded delay not supported: {op}")
            steps, n = self.sequence(op.operands[0])
            return [(x, t + d) for x, t in steps], n + d
        if op.name == "ltl.concat":
            steps, end = [], 0
            for k, x in enumerate(op.operands):
                sub, n = self.sequence(x)
                base = end
                steps += [(y, base + t) for y, t in sub]
                end = base + n
            return steps, end
        raise SystemExit(f"unsupported sequence op {op.name}")

    def check_nonoverlap(self, ante):
        """Warn about circt-verilog's encoding of `a |=> b`.

        ImportVerilog builds concat(delay(a, 1, 0), true) |-> b. With delay
        and concat as defined by the LTL dialect (delay moves the start of a
        later, concat overlaps end and start) that checks b in the cycle
        where a holds, like a |-> b, not one cycle later. This script follows
        the dialect's definition; write `a ##1 1'b1 |-> b` instead of |=>.
        """
        try:
            op = self.unwrap(ante).owner.operation
        except AttributeError:
            return
        if op.name != "ltl.concat" or len(op.operands) != 2:
            return
        try:
            d = op.operands[0].owner.operation
        except AttributeError:
            return
        if d.name == "ltl.delay" and \
                IntegerAttr(d.attributes["delay"]).value == 1 and \
                const_value(op.operands[1]) in (1, -1):
            print("warning: assertion uses |=>, which circt-verilog encodes as "
                  "|-> (see check_nonoverlap); use ##1", file=sys.stderr)

    def violation(self, op):
        prop, en = op.operands[0], (op.operands[2]
                                     if len(op.operands) > 2 else None)
        if self.clk is None:  # i1 clock (no !seq.clock port)
            self.clk = create("seq.to_clock", [op.operands[1]],
                              [Type.parse("!seq.clock")])[0]
        prop = self.unwrap(prop)
        if isinstance(prop.type, IntegerType):
            return self.and_([en, self.bnot(prop)])
        imp = prop.owner.operation
        if imp.name != "ltl.implication":
            raise SystemExit(f"unsupported property {imp.name}")
        self.dead.append(imp)
        cons = self.unwrap(imp.operands[1])
        if not isinstance(cons.type, IntegerType):
            raise SystemExit("only boolean consequents are supported")
        self.check_nonoverlap(imp.operands[0])
        steps, end = self.sequence(imp.operands[0])
        match = [self.past(x, end - t) for x, t in steps]
        ens = [self.past(en, k) for k in range(end + 1)] if en is not None \
            else []
        return self.and_(ens + match + [self.bnot(cons)])

    def run(self, keep, assume):
        body = self.top.regions[0].blocks[0]
        ops = [op.operation for op in body.operations
               if op.operation.name in ("verif.clocked_assert",
                                        "verif.clocked_assume")]
        labels, viols, assumes = [], [], []
        with InsertionPoint(self.term):
            for op in ops:
                lab = assert_label(op) or f"#{len(labels)}"
                v = self.violation(op)
                if op.name == "verif.clocked_assume" or lab in assume:
                    assumes.append((lab, v))
                else:
                    labels.append(lab)
                    viols.append(v)
            sel = [v for l, v in zip(labels, viols)
                   if keep is None or l in keep]
            bad = sel[0] if len(sel) == 1 else (
                create("comb.or", sel, [i(1)])[0] if sel else const(0, 1))
            if assumes:
                # nok: an assumption was violated before (latch starts at 0)
                anyv = assumes[0][1] if len(assumes) == 1 else create(
                    "comb.or", [v for _, v in assumes], [i(1)])[0]
                nok = compreg(const(0, 1), self.clk)
                nok_next = create("comb.or", [nok, anyv], [i(1)])[0]
                nok.owner.operation.operands[0] = nok_next
                bad = self.and_([bad, self.bnot(nok_next)])
        for op in ops:
            op.erase()
        # LTL ops left without users (shared between assertions, so dedupe
        # and erase users first; touching an erased op aborts)
        pending = []
        for op in self.dead:
            if not any(op == o for o in pending):
                pending.append(op)
        changed = True
        while changed:
            changed = False
            for op in list(pending):
                if not any(True for r in op.results for _ in r.uses):
                    pending.remove(op)
                    op.erase()
                    changed = True
        unknown = (keep or set()) - set(labels)
        if unknown:
            raise SystemExit(f"no such assertion: {sorted(unknown)}")
        return bad, labels, [l for l, _ in assumes]


def split_top(sig):
    """Split a port list at top-level commas."""
    parts, depth, cur = [], 0, ""
    for ch in sig:
        if ch in "<([{":
            depth += 1
        elif ch in ">)]}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur.strip())
    return parts


def replace_outputs(text, top):
    """Replace the output ports of the top module by `out bad : i1`."""
    head = "hw.module @" + top + "("
    a = text.index(head) + len(head)
    depth, b = 1, a
    while depth:
        depth += {"(": 1, ")": -1}.get(text[b], 0)
        b += 1
    ports = [p for p in split_top(text[a:b - 1]) if not p.startswith("out ")]
    return text[:a] + ", ".join(ports + ["out bad : i1"]) + text[b - 1:]


def symbolic_to_ports(text, top):
    """verif.symbolic_value lines become input ports of the top module."""
    ports, out = [], []
    for l in text.splitlines():
        m = re.match(r"\s*(%[\w.$-]+) = verif\.symbolic_value : (\S+)", l)
        if m:
            ports.append((m.group(1), m.group(2)))
        else:
            out.append(l)
    text = "\n".join(out) + "\n"
    rename = {old: f"%free{k}" for k, (old, _) in enumerate(ports)}
    if rename:
        pat = re.compile("(?:" + "|".join(
            re.escape(o) for o in sorted(rename, key=len, reverse=True)) +
                         r")(?![\w.$-])")
        text = pat.sub(lambda m: rename[m.group(0)], text)
    extra = "".join(f"in {rename[o]} : {t}, " for o, t in ports)
    head = re.compile(r"(hw\.module @" + re.escape(top) + r"\()")
    return head.sub(lambda m: m.group(1) + extra, text, count=1), len(ports)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--top", default=None,
                    help="module to keep (default: the only one left after "
                    "flattening)")
    ap.add_argument("--noreset", choices=["free", "zero"], default="free")
    ap.add_argument("--assert", dest="asserts", default=None,
                    help="comma-separated SVA labels to check (default: all); "
                    "with assertions present, the outputs are replaced by "
                    "one output `bad`")
    ap.add_argument("--assume", default="",
                    help="comma-separated SVA labels to use as assumptions")
    ap.add_argument("--substitute-antecedent", action="store_true",
                    help="run verif-substitute-antecedent before lowering")
    ap.add_argument("--list", action="store_true",
                    help="print the assertion labels and exit")
    args = ap.parse_args()
    with Context() as ctx, Location.unknown():
        circt.register_dialects(ctx)
        # LLHD passes are not in the Python bindings: run circt-opt.
        pre = subprocess.run(["circt-opt", args.inp, "--pass-pipeline=" + PRE.replace(
                                  "{SUBST}", SUBST if args.substitute_antecedent
                                  else ""),
                              "--mlir-print-op-generic",
                              "--mlir-print-debuginfo"],
                             check=True, capture_output=True, text=True)
        # assertions that verif-substitute-antecedent proved (and canonicalize
        # then removed) are reported as remarks at their source location
        proved = sorted({label_at(f'loc("{p}":{l}:') for p, l in re.findall(
            r'^(\S+?):(\d+):\d+: remark: consequent holds under the '
            r'antecedent', pre.stderr, re.M)})
        ctx.allow_unregistered_dialects = True  # LLHD ops, generic form
        m = Module.parse(pre.stdout)
        mods = [op for op in m.body.operations
                if op.operation.name == "hw.module"]
        if args.top:
            top = next(op for op in mods if StringAttr(
                op.attributes["sym_name"]).value == args.top)
        else:
            if len(mods) != 1:
                raise SystemExit("several modules left, pass --top")
            top = mods[0]
        name = StringAttr(top.attributes["sym_name"]).value
        nsplit, nkept = split_signals(top)
        body = top.regions[0].blocks[0]
        clk = next((a for a in body.arguments
                    if str(a.type) == "!seq.clock"), None)
        has_props = any(op.operation.name.startswith("verif.clocked_")
                        for op in body.operations)
        labels = assumed = []
        if has_props:
            keep = set(args.asserts.split(",")) if args.asserts else None
            assume = set(filter(None, args.assume.split(",")))
            for l in proved:
                print(f"{l}: proved by verif-substitute-antecedent")
            bad, labels, assumed = Props(top, clk).run(
                keep - set(proved) if keep else None, assume)
            if args.list:
                print("\n".join(labels + proved))
                return
            term = list(body.operations)[-1]
            term.operation.erase()
            with InsertionPoint(body):
                create("hw.output", [bad], [])
        low = Lower(top, args.noreset)
        n = low.run()
        for op in list(m.body.operations):
            if op.operation != top.operation:
                op.operation.erase()
        # the outputs do not match the signature yet: skip the verifier, which
        # would make the printer fall back to the generic form
        text = m.operation.get_asm(enable_debug_info=False,
                                   assume_verified=True)
        if has_props:
            text = replace_outputs(text, name)
        left = sorted(set(re.findall(r"\bllhd\.[\w.]+", text)))
    text, nfree = symbolic_to_ports(text, name)
    post = subprocess.run(["circt-opt", f"--pass-pipeline={POST}"],
                          input=text, check=True, capture_output=True,
                          text=True)
    open(args.out, "w").write(post.stdout)
    s = " ".join(f"{k}={v}" for k, v in low.stats.items() if v)
    if labels:
        print(f"assertions {len(labels)} checked "
              f"{len(labels) if not args.asserts else len(args.asserts.split(','))}"
              f" assumptions {len(assumed)}")
    sig = f" llhd-signals split {nsplit} kept {nkept}" if nsplit or nkept \
        else ""
    print(f"{name}: registers {n} ({s}) free-inputs {nfree}{sig}"
          + (f" llhd-left {','.join(left)}" if left else ""))


if __name__ == "__main__":
    main()
