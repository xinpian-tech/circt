# classify.py: attribute the members merged by signal correspondence to
# word-level structures.
#   python3 classify.py            -> study32/attribution.tsv, attribution-summary.md
#   python3 classify.py D C VAR N  -> print N examples per category (debugging)
#
# Input per check (attr_run.sh): <c>.l.mlir, the word-level IR with every op
# tagged loc("w":LINE:0), and <c>.{noinit,init}.md, the classes sc proved on
# the bit-level model lowered from it, without (noinit) and with (init) the
# registers' initial values. A class is "class N [const]: m0 m1 ...", members
# G@LINE (an and-gate lowered from the word op on LINE; bare G: fused or no
# location), a register bit by name, `~` for the inverted phase. m0 is the
# representative; the others are merged into it (all members of a constant
# class are merged into the constant).
#
# Every merged member gets exactly one category, by the first rule that
# applies (M = word op of the member, R = word op of the representative):
#  1 const-reg      constant class, member is a register bit
#  2 const-gate     constant class, member is a gate
#  3 reg-sva        member is a register bit and the member or the
#                   representative is named *_sva* (assertion shadow state)
#  4 reg-design     member is a register bit (other register equivalence;
#                   includes unnamed registers, which print with an empty name)
#  5 unlocated      member gate without a location (fused) or whose line is
#                   not an op line (e.g. the module's own location)
#  6 reset-mux      M is a mux whose condition reads the reset (cv32: input
#                   rst_ni, pico: register reset) through at most 3
#                   and/or/xor ops and one of whose data arms is a constant
#  7 first-mux      M is a mux whose condition reads the harness' first-cycle
#                   register `first` through at most 3 and/or/xor ops (the
#                   harness' symbolic initial values, `first ? reg : sym`)
#  8 regfile-slot   M is a mux that feeds (through at most 2 muxes) a
#                   register named *cpuregs*/*register_file*/*regfile*, or a
#                   write slot: one of its non-constant data arms (the write
#                   data) is shared by muxes feeding (through at most 2
#                   muxes) at least 8 different registers
#  9 arith-same     M and R are the same add/sub/mul/div/mod/shift or ordering
#                   comparison (bits of one arithmetic op, e.g. carries)
# 10 arith-cross    M or R is such an arithmetic op (different ops: e.g. the
#                   xor or equality test of two operands vs. their difference)
# 11 gate-eq-reg    the representative is a register bit (a gate equal to a
#                   register: next-state / hold logic)
# 12 decode-compare M and R are icmp (eq/ne)/and/or/xor (R: or unlocated):
#                   equality tests, priority decoders, their conjunctions
# 13 mux-select     M is a mux and R is reachable from a data arm of M through
#                   at most 3 muxes (redundant select: the mux, or a chain of
#                   muxes, reduces to one of its arms; its condition is
#                   implied by the context)
# 14 mux-dup        M or R is a mux otherwise (selects equal to another
#                   select or gate built separately: duplicated mux trees)
# 15 other          anything else (e.g. concat/extract-only ops)
import collections, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wordir

D = '/root/yjh-workspace/solver-oriented-out/study32'
CATS = ['const-reg', 'const-gate', 'reg-sva', 'reg-design', 'unlocated', 'reset-mux', 'first-mux',
        'regfile-slot', 'arith-same', 'arith-cross', 'gate-eq-reg', 'decode-compare', 'mux-select', 'mux-dup', 'other']
ARITH = {'comb.add', 'comb.sub', 'comb.mul', 'comb.divu', 'comb.divs', 'comb.modu', 'comb.mods',
         'comb.shl', 'comb.shru', 'comb.shrs'}
ORDER = {'slt', 'sle', 'sgt', 'sge', 'ult', 'ule', 'ugt', 'uge'}
LOGIC = {'comb.and', 'comb.or', 'comb.xor', 'comb.parity'}
# Conditions are traced through boolean logic only: every register read goes
# through a mux on rst_ni (cv32's reset at the register outputs) or on first
# (pico's symbolic initial values), so tracing through muxes would reach them
# from almost every condition.
THROUGH = {'comb.xor', 'comb.and', 'comb.or'}
RESET = {'cv32': {'%rst_ni'}, 'pico': {'%reset'}}
REGFILE = re.compile(r'cpuregs|register_file|regfile', re.I)


def is_arith(o):
    return o is not None and (o.name in ARITH or (o.name == 'comb.icmp' and o.pred in ORDER))


class Classifier:
    def __init__(self, design, check):
        self.design = design
        self.W = wordir.WordIR(f'{D}/attr/{design}/{check}.l.mlir')
        self.memo = {}
        self._slots()

    def _slots(self):
        # Register-file slots: muxes feeding a register through <= 2 muxes,
        # grouped by data arm; arms shared by >= 8 muxes feeding distinct
        # registers mark write slots.
        W = self.W
        byarm = collections.defaultdict(set)
        feeds = {}
        for o in W.ops.values():
            if o.name != 'comb.mux' or len(o.operands) < 3:
                continue
            regs = W.reg_users(o, 2)
            feeds[o.line] = regs
            if regs:
                for a in o.operands[1:3]:
                    if not W.is_const(a):
                        byarm[a].add((o.line, tuple(sorted(regs))[0]))
        self.slot = set()
        for a, s in byarm.items():
            if len({r for _, r in s}) >= 8:
                self.slot |= {l for l, _ in s}
        for l, regs in feeds.items():
            if any(REGFILE.search(r) for r in regs):
                self.slot.add(l)

    def mux_kind(self, o):
        W = self.W
        if o.line in self.memo:
            return self.memo[o.line]
        k = None
        c = o.operands[0]
        if W.cone_reaches(c, RESET[self.design], 3, THROUGH) and any(W.is_const(a) for a in o.operands[1:3]):
            k = 'reset-mux'
        elif W.cone_reaches(c, {'%first'}, 3, THROUGH):
            k = 'first-mux'
        elif o.line in self.slot:
            k = 'regfile-slot'
        self.memo[o.line] = k
        return k

    def arm_reaches(self, M, r):
        stack = [(a, 3) for a in M.operands[1:3]]
        while stack:
            v, d = stack.pop()
            if v == r:
                return True
            o = self.W.defs.get(v)
            if d > 0 and o is not None and o.name == 'comb.mux' and len(o.operands) >= 3:
                stack += [(a, d - 1) for a in o.operands[1:3]]
        return False

    def classify(self, const, member, rep):
        inv, kind, val = member
        if const:
            return 'const-reg' if kind == 'R' else 'const-gate'
        if kind == 'R':
            if '_sva' in val or (rep[1] == 'R' and '_sva' in rep[2]):
                return 'reg-sva'
            return 'reg-design'
        M = self.W.ops.get(val) if val is not None else None
        if M is None:
            return 'unlocated'
        R = self.W.ops.get(rep[2]) if rep[1] == 'G' and rep[2] is not None else None
        if M.name == 'comb.mux' and len(M.operands) >= 3:
            k = self.mux_kind(M)
            if k:
                return k
        if is_arith(M) and R is not None and R.line == M.line:
            return 'arith-same'
        if is_arith(M) or is_arith(R):
            return 'arith-cross'
        if rep[1] == 'R':
            return 'gate-eq-reg'
        def dc(o):
            return o is not None and (o.name in LOGIC or (o.name == 'comb.icmp' and o.pred not in ORDER))
        # An unlocated representative: by M alone.
        if dc(M) and (dc(R) or R is None):
            return 'decode-compare'
        if M.name == 'comb.mux' and R is not None and self.arm_reaches(M, R.res):
            return 'mux-select'
        if M.name == 'comb.mux' or (R is not None and R.name == 'comb.mux'):
            return 'mux-dup'
        return 'other'

    def run(self, variant, examples=0):
        """Category counts, examples, and the categories per source (the
        member's word op line, or its register bit name)."""
        cnt = collections.Counter()
        ex = collections.defaultdict(list)
        by = collections.defaultdict(collections.Counter)
        for const, mem in wordir.classes(f'{D}/attr/{self.design}/{self.check}.{variant}.md'):
            rep = mem[0]
            for m in (mem if const else mem[1:]):
                k = self.classify(const, m, rep)
                cnt[k] += 1
                by[(m[1], m[2])][k] += 1
                if examples and len(ex[k]) < examples:
                    ex[k].append((m, rep))
        return cnt, ex, by


def new_merges(a, b):
    """The merges of the init run beyond the noinit run, by source: for each
    source (word op line / register bit) with n0 merged members without and
    n1 > n0 with initial values, n1 - n0 new merges, given the categories the
    init run has there and the noinit run has not (category multiset
    difference, in CATS order), then any others in CATS order. Also the
    number of merges lost (sources with n1 < n0)."""
    new = collections.Counter(); lost = 0
    for key in set(a) | set(b):
        n0, n1 = sum(a[key].values()), sum(b[key].values())
        if n1 < n0:
            lost += n0 - n1
        if n1 <= n0:
            continue
        left = n1 - n0
        diff = b[key] - a[key]
        for pool in (diff, b[key]):
            for k in CATS:
                t = min(left, pool[k])
                new[k] += t; left -= t
        assert left == 0
    return new, lost


def done(design, check):
    p = f'{D}/attr/{design}/{check}'
    ok = True
    for v in ['noinit', 'init']:
        t = f'{p}.{v}.time'
        if not (os.path.exists(t) and open(t).read().split()[0] == '0'):
            ok = False
    return ok


def main():
    rows = []
    for design in ['cv32', 'pico']:
        ad = f'{D}/attr/{design}'
        checks = sorted(f[:-len('.l.mlir')] for f in os.listdir(ad) if f.endswith('.l.mlir'))
        for c in checks:
            if not done(design, c):
                continue
            cl = Classifier(design, c); cl.check = c
            a, _, ba = cl.run('noinit')
            b, _, bb = cl.run('init')
            n, lost = new_merges(ba, bb)
            rows.append((design, c, a, b, n, lost))
    with open(f'{D}/attribution.tsv', 'w') as f:
        f.write('design\tcheck\tpart\ttotal\tlost\t' + '\t'.join(CATS) + '\n')
        for design, c, a, b, n, lost in rows:
            for part, x in [('comb', a), ('all', b), ('new', n)]:
                f.write(f'{design}\t{c}\t{part}\t{sum(x[k] for k in CATS)}\t{lost if part == "new" else ""}\t' +
                        '\t'.join(str(x[k]) for k in CATS) + '\n')
    out = []
    for design in ['cv32', 'pico']:
        R = [r for r in rows if r[0] == design]
        if not R:
            continue
        A = collections.Counter(); B = collections.Counter(); N = collections.Counter(); L = 0
        for _, _, a, b, n, lost in R:
            A.update(a); B.update(b); N.update(n); L += lost
        ta, tb, tn = sum(A.values()), sum(B.values()), sum(N.values())
        out.append(f'### {design} ({len(R)} checks)\n')
        out.append(f'Merged members: {ta} without initial values (comb: true in every state), {tb} with '
                   f'(all); {tn} new merges with initial values (sources merged more often with than without), '
                   f'{L} lost (sources merged less often: their gates are covered by other classes, e.g. '
                   f'a gate merged into another gate without initial values and constant with them).\n')
        out.append('| category | comb | % of comb | new (induction) | % of new | all | % of all |')
        out.append('|---|---|---|---|---|---|---|')
        for k in sorted(CATS, key=lambda k: -B[k]):
            if not (A[k] or B[k] or N[k]):
                continue
            out.append(f'| {k} | {A[k]} | {100*A[k]/ta:.1f}% | {N[k]} | {100*N[k]/tn if tn else 0:.1f}% | '
                       f'{B[k]} | {100*B[k]/tb:.1f}% |')
        out.append('')
    open(f'{D}/attribution-summary.md', 'w').write('\n'.join(out) + '\n')
    print('\n'.join(out))


def const_ops():
    """Secondary table: the word ops of the constant gates (init run)."""
    out = ['| design | word op of the constant gate | members | % |', '|---|---|---|---|']
    for design in ['cv32', 'pico']:
        ad = f'{D}/attr/{design}'
        cnt = collections.Counter()
        for c in sorted(f[:-len('.l.mlir')] for f in os.listdir(ad) if f.endswith('.l.mlir')):
            if not done(design, c):
                continue
            cl = Classifier(design, c); cl.check = c
            for const, mem in wordir.classes(f'{ad}/{c}.init.md'):
                if not const:
                    continue
                for m in mem:
                    if m[1] != 'G':
                        continue
                    o = cl.W.ops.get(m[2]) if m[2] is not None else None
                    if o is None:
                        k = 'unlocated'
                    elif o.name == 'comb.mux':
                        k = {'reset-mux': 'mux on reset', 'first-mux': 'mux on first'}.get(cl.mux_kind(o), 'mux')
                    elif is_arith(o):
                        k = 'arithmetic'
                    else:
                        k = o.name.replace('comb.', '') + (f' {o.pred}' if o.pred else '')
                    cnt[k] += 1
        t = sum(cnt.values())
        for k, n in cnt.most_common(8):
            out.append(f'| {design} | {k} | {n} | {100*n/t:.1f}% |')
    s = '\n'.join(out) + '\n'
    open(f'{D}/const-ops.md', 'w').write(s)
    print(s)


if __name__ == '__main__':
    if len(sys.argv) == 5:
        cl = Classifier(sys.argv[1], sys.argv[2]); cl.check = sys.argv[2]
        cnt, ex, _ = cl.run(sys.argv[3], int(sys.argv[4]))
        for k, n in cnt.most_common():
            print(n, k)
            for m, rep in ex[k]:
                for x in (m, rep):
                    o = cl.W.ops.get(x[2]) if x[1] == 'G' and x[2] else None
                    print('    ', ''.join(map(str, x)), (o.res + ' = ' + o.name + o.rest[:110]) if o else '')
                print()
    else:
        main()
        const_ops()
