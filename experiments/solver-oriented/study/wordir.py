# wordir.py: the tagged word-level IR of one check (study32/attr/<d>/<c>.l.mlir)
# and the merge-dump classes of signal correspondence on its bit-level model.
import re

OPRE = re.compile(r'^\s*(%[\w.$-]+)(?::\d+)? = ([\w.]+)(.*?)\s+loc\("w":(\d+):0\)\s*$')
NAMERE = re.compile(r'%[\w.$-]+')


class Op:
    __slots__ = ('line', 'res', 'name', 'rest', 'operands', 'width', 'pred', 'regname')

    def __init__(self, line, res, name, rest):
        self.line, self.res, self.name, self.rest = line, res, name, rest
        body = re.sub(r'\{[^{}]*\}', '', rest)      # drop attribute dicts
        body = re.sub(r'name "[^"]*"', '', body)
        self.operands = NAMERE.findall(body.split(' : ')[0] if ' : ' in body else body)
        m = re.search(r'(?:->|:)\s*\(?i(\d+)\)?\s*$', rest)
        self.width = int(m.group(1)) if m else 0
        m = re.match(r'\s*(eq|ne|slt|sle|sgt|sge|ult|ule|ugt|uge|ceq|cne|weq|wne)\b', rest)
        self.pred = m.group(1) if (name == 'comb.icmp' and m) else None
        m = re.search(r'name "([^"]*)"', rest)
        self.regname = m.group(1) if m else (res[1:] if name == 'seq.compreg' else None)

    def __repr__(self):
        return f'{self.line}:{self.name}{"/" + self.pred if self.pred else ""}'


class WordIR:
    def __init__(self, path):
        self.ops = {}       # line -> Op
        self.defs = {}      # ssa name -> Op
        self.inputs = set()
        self.users = {}     # ssa name -> [Op]
        for i, l in enumerate(open(path), 1):
            if 'hw.module @' in l:
                self.inputs |= set(re.findall(r'in (%[\w.$-]+) :', l))
            m = OPRE.match(l)
            if not m:
                continue
            op = Op(int(m.group(4)), m.group(1), m.group(2), m.group(3))
            assert op.line == i, (path, i)
            self.ops[op.line] = op
            self.defs[op.res] = op
        for op in self.ops.values():
            for v in op.operands:
                self.users.setdefault(v, []).append(op)

    def op(self, v):
        return self.defs.get(v)

    def is_const(self, v):
        o = self.defs.get(v)
        return o is not None and o.name == 'hw.constant'

    def cone_reaches(self, v, targets, depth, through=None):
        """v depends on one of `targets` (ssa names) within `depth` ops."""
        seen = set()
        stack = [(v, depth)]
        while stack:
            x, d = stack.pop()
            if x in targets:
                return True
            if d == 0 or x in seen:
                continue
            seen.add(x)
            o = self.defs.get(x)
            if o is None or o.name == 'seq.compreg':
                continue
            if through and o.name not in through:
                continue
            for y in o.operands:
                stack.append((y, d - 1))
        return False

    def reg_users(self, op, depth=4):
        """Registers whose next state reads `op` through at most `depth` muxes/concats/extracts."""
        regs = set()
        stack = [(op.res, depth)]
        seen = set()
        while stack:
            v, d = stack.pop()
            if v in seen:
                continue
            seen.add(v)
            for u in self.users.get(v, []):
                if u.name == 'seq.compreg' and u.operands and u.operands[0] == v:
                    regs.add(u.regname or u.res)
                elif d > 0 and u.name in ('comb.mux', 'comb.concat', 'comb.extract', 'comb.replicate'):
                    stack.append((u.res, d - 1))
        return regs


MEMRE = re.compile(r'(~?)(\S+)')


def classes(path):
    """[(is_const, [(inverted, kind, value)])]; kind 'G' (value: line or None)
    or 'R' (value: register bit name, '' if unnamed). The first member is the representative."""
    out = []
    for l in open(path):
        if not l.startswith('class '):
            continue
        head, _, body = l.rstrip('\n').partition(': ')
        const = head.endswith(' const')
        mem = []
        # One member per space-separated token; an unnamed register prints as
        # an empty name (an empty token, or `~` alone when inverted).
        for tok in body.split(' '):
            inv = tok.startswith('~')
            t = tok[1:] if inv else tok
            if t == 'G':
                mem.append((inv, 'G', None))
            elif t.startswith('G@'):
                mem.append((inv, 'G', int(t[2:])))
            else:
                mem.append((inv, 'R', t))
        out.append((const, mem))
    return out
