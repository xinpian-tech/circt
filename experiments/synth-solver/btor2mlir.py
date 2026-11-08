#!/usr/bin/env python3
"""BTOR2 (bit-vector, no arrays) to HW/Comb/Seq MLIR with one `bad` output.

The result is meant for CIRCT's AIGER export, which has no latch initial
values, constraints or multiple properties, so these are encoded:

- every state is a register `L` with initial value 0 (AIGER default); a
  register `first` (0 at time 0, then 1) selects the state's value:
  s = first ? L : init, where init is the BTOR2 init expression, or a fresh
  input for an uninitialized state; L' = next(s). States whose init is the
  constant 0 skip the mux (s = L);
- constraints: a register `ok` (stored inverted so it starts at 1) keeps
  whether all constraints held so far: ok' = ok & c; bad = ok & c & (b1|b2..).

Division by zero follows BTOR2 (SMT-LIB) semantics through explicit muxes.

    btor2mlir.py IN.btor2 OUT.mlir
"""

import sys


class Gen:

    def __init__(self):
        self.lines = []
        self.n = 0
        self.inputs = []  # (name, width)

    def fresh(self):
        self.n += 1
        return f"%t{self.n}"

    def emit(self, rhs):
        v = self.fresh()
        self.lines.append(f"  {v} = {rhs}")
        return v

    def const(self, val, w):
        val %= 1 << w
        if w == 1:
            return self.emit(f"hw.constant {'true' if val else 'false'}")
        # print as signed to stay inside the attribute's range
        if val >= 1 << (w - 1):
            val -= 1 << w
        return self.emit(f"hw.constant {val} : i{w}")

    def op(self, name, args, w):
        return self.emit(f"comb.{name} {', '.join(args)} : i{w}")

    def icmp(self, pred, a, b, w):
        return self.emit(f"comb.icmp {pred} {a}, {b} : i{w}")

    def mux(self, s, a, b, w):
        return self.emit(f"comb.mux {s}, {a}, {b} : i{w}")

    def extract(self, a, lo, w_out, w_in):
        if lo == 0 and w_out == w_in:
            return a
        return self.emit(f"comb.extract {a} from {lo} : (i{w_in}) -> i{w_out}")

    def concat(self, parts):
        """parts: list of (value, width), most significant first."""
        if len(parts) == 1:
            return parts[0][0]
        tys = ", ".join(f"i{w}" for _, w in parts)
        return self.emit(
            f"comb.concat {', '.join(v for v, _ in parts)} : {tys}")

    def replicate(self, a, n):
        if n == 1:
            return a
        return self.emit(f"comb.replicate {a} : (i1) -> i{n}")

    def bnot(self, a, w):
        return self.op("xor", [a, self.const(-1, w)], w)


def convert(text):
    g = Gen()
    sorts = {}
    val = {}  # id -> (value, width)
    states = {}  # id -> width
    init, nxt = {}, {}
    bads, constraints = [], []
    pending = []  # node lines, processed in order

    def lit(t):
        i = int(t)
        v, w = val[abs(i)]
        return (g.bnot(v, w), w) if i < 0 else (v, w)

    lines = [l.split(";")[0].split() for l in text.splitlines()]
    lines = [l for l in lines if l]
    # Pass 1: sorts, states, inputs, init/next (states may be used before
    # their defining lines are processed).
    for l in lines:
        nid, kind = int(l[0]), l[1]
        if kind == "sort":
            if l[2] != "bitvec":
                raise SystemExit("arrays are not supported")
            sorts[nid] = int(l[3])
        elif kind == "state":
            states[nid] = sorts[int(l[2])]
        elif kind == "init":
            init[int(l[3])] = l[4]
        elif kind == "next":
            nxt[int(l[3])] = l[4]
    # A state reads its register directly when its init is the constant 0,
    # else S = first ? L : init (a fresh input when uninitialized).
    zeros = set()
    for l in lines:
        if l[1] == "zero" or (l[1] in ("const", "constd", "consth") and
                              int(l[3], {"const": 2, "constd": 10,
                                         "consth": 16}[l[1]]) == 0):
            zeros.add(l[0])
    direct = {s for s in states if init.get(s) in zeros}
    for s, w in states.items():
        val[s] = (f"%L{s}" if s in direct else f"%S{s}", w)
    for l in lines:
        nid, kind = int(l[0]), l[1]
        if kind in ("sort", "state", "init", "next"):
            continue
        if kind == "input":
            w = sorts[int(l[2])]
            g.inputs.append((f"i{nid}", w))
            val[nid] = (f"%i{nid}", w)
            continue
        if kind in ("bad", "constraint", "output", "fair", "justice"):
            if kind == "bad":
                bads.append(l[2])
            elif kind == "constraint":
                constraints.append(l[2])
            continue
        w = sorts[int(l[2])]
        a = l[3:]
        if kind in ("const", "constd", "consth"):
            base = {"const": 2, "constd": 10, "consth": 16}[kind]
            val[nid] = (g.const(int(a[0], base), w), w)
        elif kind == "zero":
            val[nid] = (g.const(0, w), w)
        elif kind == "one":
            val[nid] = (g.const(1, w), w)
        elif kind == "ones":
            val[nid] = (g.const(-1, w), w)
        elif kind == "not":
            x, _ = lit(a[0])
            val[nid] = (g.bnot(x, w), w)
        elif kind in ("inc", "dec"):
            x, _ = lit(a[0])
            val[nid] = (g.op("add" if kind == "inc" else "sub",
                             [x, g.const(1, w)], w), w)
        elif kind == "neg":
            x, _ = lit(a[0])
            val[nid] = (g.op("sub", [g.const(0, w), x], w), w)
        elif kind in ("redand", "redor", "redxor"):
            x, wx = lit(a[0])
            if kind == "redand":
                v = g.icmp("eq", x, g.const(-1, wx), wx)
            elif kind == "redor":
                v = g.icmp("ne", x, g.const(0, wx), wx)
            else:
                v = g.emit(f"comb.parity {x} : i{wx}")
            val[nid] = (v, 1)
        elif kind in ("sext", "uext"):
            x, wx = lit(a[0])
            k = int(a[1])
            if k == 0:
                val[nid] = (x, w)
            elif kind == "uext":
                val[nid] = (g.concat([(g.const(0, k), k), (x, wx)]), w)
            else:
                msb = g.extract(x, wx - 1, 1, wx)
                val[nid] = (g.concat([(g.replicate(msb, k), k), (x, wx)]), w)
        elif kind == "slice":
            x, wx = lit(a[0])
            hi, lo = int(a[1]), int(a[2])
            val[nid] = (g.extract(x, lo, hi - lo + 1, wx), w)
        elif kind == "ite":
            c, _ = lit(a[0])
            x, _ = lit(a[1])
            y, _ = lit(a[2])
            val[nid] = (g.mux(c, x, y, w), w)
        else:
            x, wx = lit(a[0])
            y, wy = lit(a[1])
            val[nid] = (binary(g, kind, x, y, wx, wy, w), w)

    # Bind states and registers.
    regs = []
    for s, w in states.items():
        if s not in direct:
            if s in init:
                iv, _ = lit(init[s])
            else:
                g.inputs.append((f"u{s}", w))
                iv = f"%u{s}"
            g.lines.append(f"  %S{s} = comb.mux %first, %L{s}, {iv} : i{w}")
        if s in nxt:
            nv, _ = lit(nxt[s])
        else:  # no next: an arbitrary value each step
            g.inputs.append((f"x{s}", w))
            nv = f"%x{s}"
        regs.append(f"  %L{s} = seq.compreg {nv}, %clk : i{w}")
    t = g.const(1, 1)
    if len(direct) < len(states):
        regs.append(f"  %first = seq.compreg {t}, %clk : i1")
    g.lines += regs
    # Constraints and bad.
    def conj(vs, op):
        acc = vs[0]
        for v in vs[1:]:
            acc = g.op(op, [acc, v], 1)
        return acc

    bad = conj([lit(b)[0] for b in bads], "or")
    if constraints:
        c_all = conj([lit(c)[0] for c in constraints], "and")
        ok = g.bnot("%nok", 1)
        ok_c = g.op("and", [ok, c_all], 1)
        g.lines.append(f"  %nok = seq.compreg {g.bnot(ok_c, 1)}, %clk : i1")
        bad = g.op("and", [ok_c, bad], 1)
    ins = "".join(f", in %{n}: i{w}" for n, w in g.inputs)
    head = f"hw.module @top(in %clk: !seq.clock{ins}, out bad: i1) {{"
    return "\n".join([head] + g.lines + [f"  hw.output {bad} : i1", "}"]) + "\n"


def binary(g, kind, x, y, wx, wy, w):
    cmp = {"eq": "eq", "neq": "ne", "sgt": "sgt", "sgte": "sge",
           "slt": "slt", "slte": "sle", "ugt": "ugt", "ugte": "uge",
           "ult": "ult", "ulte": "ule"}
    if kind in cmp:
        return g.icmp(cmp[kind], x, y, wx)
    if kind == "iff":
        return g.icmp("eq", x, y, 1)
    if kind == "implies":
        return g.op("or", [g.bnot(x, 1), y], 1)
    simple = {"and": "and", "or": "or", "xor": "xor", "add": "add",
              "mul": "mul", "sub": "sub", "sll": "shl", "srl": "shru",
              "sra": "shrs"}
    if kind in simple:
        return g.op(simple[kind], [x, y], w)
    if kind in ("nand", "nor", "xnor"):
        return g.bnot(g.op(kind[1:] if kind != "xnor" else "xor", [x, y], w),
                      w)
    if kind == "concat":
        return g.concat([(x, wx), (y, wy)])
    if kind in ("rol", "ror"):
        # rotate by y mod w
        wv = g.const(w, w)
        k = g.op("modu", [y, wv], w) if w & (w - 1) else g.op(
            "and", [y, g.const(w - 1, w)], w)
        inv = g.op("sub", [wv, k], w)
        a, b = ("shl", "shru") if kind == "rol" else ("shru", "shl")
        r = g.op("or", [g.op(a, [x, k], w), g.op(b, [x, inv], w)], w)
        return g.mux(g.icmp("eq", k, g.const(0, w), w), x, r, w)
    zero = g.const(0, w)
    bz = g.icmp("eq", y, zero, w)
    if kind == "udiv":
        return g.mux(bz, g.const(-1, w), g.op("divu", [x, y], w), w)
    if kind == "urem":
        return g.mux(bz, x, g.op("modu", [x, y], w), w)
    if kind == "sdiv":
        neg = g.icmp("slt", x, zero, w)
        dz = g.mux(neg, g.const(1, w), g.const(-1, w), w)
        return g.mux(bz, dz, g.op("divs", [x, y], w), w)
    if kind == "srem":
        return g.mux(bz, x, g.op("mods", [x, y], w), w)
    if kind == "smod":
        r = g.op("mods", [x, y], w)
        rz = g.icmp("eq", r, zero, w)
        sr = g.icmp("slt", r, zero, w)
        sy = g.icmp("slt", y, zero, w)
        diff = g.op("xor", [sr, sy], 1)
        fix = g.mux(g.op("and", [diff, g.bnot(rz, 1)], 1),
                    g.op("add", [r, y], w), r, w)
        return g.mux(bz, x, fix, w)
    if kind in ("uaddo", "usubo", "umulo", "saddo", "ssubo", "smulo",
                "sdivo"):
        return overflow(g, kind, x, y, wx)
    raise SystemExit(f"unsupported op {kind}")


def overflow(g, kind, x, y, w):
    if kind == "sdivo":
        mn = g.const(1 << (w - 1), w)
        return g.op("and", [g.icmp("eq", x, mn, w),
                            g.icmp("eq", y, g.const(-1, w), w)], 1)
    signed = kind[0] == "s"

    def ext(v):
        if signed:
            msb = g.extract(v, w - 1, 1, w)
            return g.concat([(g.replicate(msb, w), w), (v, w)])
        return g.concat([(g.const(0, w), w), (v, w)])

    op = {"add": "add", "sub": "sub", "mul": "mul"}[kind[1:4]]
    r = g.op(op, [ext(x), ext(y)], 2 * w)
    lo = g.extract(r, 0, w, 2 * w)
    return g.icmp("ne", r, ext(lo), 2 * w)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    open(sys.argv[2], "w").write(convert(open(sys.argv[1]).read()))
