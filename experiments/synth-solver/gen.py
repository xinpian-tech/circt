#!/usr/bin/env python3
"""Generate small sequential models as HW/Comb/Seq MLIR.

Most families compare two implementations of the same word-level function
(the CombRewriter rewrite patterns, ASP-DAC 2026): both feed an accumulator
every cycle from free inputs, and bad = (accA != accB). The property holds,
and its inductive invariant is accA == accB, so every IC3/k-induction query
contains a miter of the two implementations. `counter` is a control-only
model with a comparator invariant.

    gen.py FAMILY WIDTH > model.mlir
"""

import sys

HEADER = "hw.module @top(in %clk: !seq.clock{ins}, out bad: i1) {{\n"


def miter(w, inputs, body_a, body_b):
    """Accumulator miter: body_x(w) returns (lines, result value name)."""
    ins = "".join(f", in %{n}: {t}" for n, t in inputs)
    out = [HEADER.format(ins=ins)]
    la, ra = body_a
    lb, rb = body_b
    out += [f"  {l}\n" for l in la + lb]
    out.append(f"  %accA = seq.compreg %nA, %clk : i{w}\n")
    out.append(f"  %accB = seq.compreg %nB, %clk : i{w}\n")
    out.append(f"  %nA = comb.add %accA, {ra} : i{w}\n")
    out.append(f"  %nB = comb.add %accB, {rb} : i{w}\n")
    out.append(f"  %bad = comb.icmp ne %accA, %accB : i{w}\n")
    out.append("  hw.output %bad : i1\n}\n")
    return "".join(out)


def family(name, w):
    i = f"i{w}"
    word = lambda *ns: [(n, i) for n in ns]
    sel = [("s", "i1")]
    if name == "mulcomm":
        # x*y vs y*x: equal by commutativity.
        return miter(w, word("x", "y"),
                     ([f"%pa = comb.mul %x, %y : {i}"], "%pa"),
                     ([f"%pb = comb.mul %y, %x : {i}"], "%pb"))
    if name == "hoist":
        # MUX relocation: s ? a*b*c : a*b*d  vs  (a*b) * (s ? c : d).
        return miter(w, sel + word("a", "b", "c", "d"),
                     ([f"%a1 = comb.mul %a, %b : {i}",
                       f"%a2 = comb.mul %a1, %c : {i}",
                       f"%a3 = comb.mul %a, %b : {i}",
                       f"%a4 = comb.mul %a3, %d : {i}",
                       f"%a5 = comb.mux %s, %a2, %a4 : {i}"], "%a5"),
                     ([f"%b1 = comb.mux %s, %c, %d : {i}",
                       f"%b2 = comb.mul %a, %b : {i}",
                       f"%b3 = comb.mul %b2, %b1 : {i}"], "%b3"))
    if name == "exch":
        # Arithmetic logic exchange: s ? a+b : c  vs  (s?a:c) + (s?b:0).
        return miter(w, sel + word("a", "b", "c"),
                     ([f"%a1 = comb.add %a, %b : {i}",
                       f"%a2 = comb.mux %s, %a1, %c : {i}"], "%a2"),
                     ([f"%z = hw.constant 0 : {i}",
                       f"%b1 = comb.mux %s, %a, %c : {i}",
                       f"%b2 = comb.mux %s, %b, %z : {i}",
                       f"%b3 = comb.add %b1, %b2 : {i}"], "%b3"))
    if name == "xorintro":
        # XOR introduction: s ? a*c : ~(b*c)  vs  ((s?a:b)*c) ^ rep(~s).
        return miter(w, sel + word("a", "b", "c"),
                     ([f"%ones = hw.constant -1 : {i}",
                       f"%a1 = comb.mul %a, %c : {i}",
                       f"%a2 = comb.mul %b, %c : {i}",
                       f"%a3 = comb.xor %a2, %ones : {i}",
                       f"%a4 = comb.mux %s, %a1, %a3 : {i}"], "%a4"),
                     (["%t = hw.constant true",
                       "%ns = comb.xor %s, %t : i1",
                       f"%b0 = comb.replicate %ns : (i1) -> {i}",
                       f"%b1 = comb.mux %s, %a, %b : {i}",
                       f"%b2 = comb.mul %b1, %c : {i}",
                       f"%b3 = comb.xor %b2, %b0 : {i}"], "%b3"))
    if name == "reorder":
        # Operation reordering: ((s?c:d)*b)*a  vs  ((s?c:d)*a)*b.
        return miter(w, sel + word("a", "b", "c", "d"),
                     ([f"%m = comb.mux %s, %c, %d : {i}",
                       f"%a1 = comb.mul %m, %b : {i}",
                       f"%a2 = comb.mul %a1, %a : {i}"], "%a2"),
                     ([f"%mb = comb.mux %s, %c, %d : {i}",
                       f"%b1 = comb.mul %mb, %a : {i}",
                       f"%b2 = comb.mul %b1, %b : {i}"], "%b2"))
    if name == "shift":
        # Shift distribution: (s?a:c) << (s?b:d)  vs  s ? a<<b : c<<d.
        return miter(w, sel + word("a", "b", "c", "d"),
                     ([f"%a1 = comb.mux %s, %a, %c : {i}",
                       f"%a2 = comb.mux %s, %b, %d : {i}",
                       f"%a3 = comb.shl %a1, %a2 : {i}"], "%a3"),
                     ([f"%b1 = comb.shl %a, %b : {i}",
                       f"%b2 = comb.shl %c, %d : {i}",
                       f"%b3 = comb.mux %s, %b1, %b2 : {i}"], "%b3"))
    if name == "counter":
        # Wrapping counter to N = 2^w - 3; bad = cnt > N.
        n = (1 << w) - 3
        return (
            HEADER.format(ins=", in %en: i1") +
            f"  %N = hw.constant {n} : {i}\n"
            f"  %one = hw.constant 1 : {i}\n"
            f"  %zero = hw.constant 0 : {i}\n"
            f"  %cnt = seq.compreg %nx, %clk : {i}\n"
            f"  %inc = comb.add %cnt, %one : {i}\n"
            f"  %top = comb.icmp eq %cnt, %N : {i}\n"
            f"  %wr = comb.mux %top, %zero, %inc : {i}\n"
            f"  %nx = comb.mux %en, %wr, %cnt : {i}\n"
            f"  %bad = comb.icmp ugt %cnt, %N : {i}\n"
            "  hw.output %bad : i1\n}\n")
    raise SystemExit(f"unknown family {name}")


FAMILIES = ["mulcomm", "hoist", "exch", "xorintro", "reorder", "shift",
            "counter"]

if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    sys.stdout.write(family(sys.argv[1], int(sys.argv[2])))
