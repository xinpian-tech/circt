#!/usr/bin/env python3
"""Solver-oriented word-level canonicalization (prototype).

Reads the flat HW/Comb/Seq subset that gen.py writes, rebuilds it as a
hash-consed DAG under rewrite rules, and prints it back. The aim is not a
smaller circuit but one where equivalent implementations become the same
nodes, while keeping shared sub-terms (no n-ary flattening in the output):

- commutative-associative ops (add, mul, and, or, xor) are flattened only to
  compute a canonical form, then rebuilt as a left-deep binary chain over
  their sorted operands, so equal operand multisets give the same node;
- mux push-down: mux(s, f(x1..xn), f(y1..yn)) -> f(mux(s,x1,y1), ...) for
  any op f but mux (applied recursively; mux(s,x,x) = x);
- identity push-down: mux(s, x+y, z) -> mux(s,x,z) + mux(s,y,0) when z is
  not an add (same for xor and or);
- negation: mux(s, x, ~y) -> mux(s,x,y) ^ rep(~s), with ~y = y ^ -1.

Each rule can be switched off (--no-RULE) for ablation.

    solvercanon.py [--check] [--no-push] [--no-ident] [--no-not] [--no-assoc] IN OUT

`--check` compares one step (next states and bad) of the input with and
without the rules on random inputs and register values.
"""

import re
import sys

sys.setrecursionlimit(1000000)

COMMUTATIVE = {"add", "mul", "and", "or", "xor"}
IDENTITY = {"add": 0, "xor": 0, "or": 0}


class Dag:

  def __init__(self, rules):
    self.rules = rules
    self.nodes = []  # (kind, op, args, width, extra)
    self.table = {}
    self.muxes = {}  # (s, x, y) -> result of mux()

  def node(self, kind, op, args, w, extra=None):
    key = (kind, op, tuple(args), w, extra)
    if key not in self.table:
      self.table[key] = len(self.nodes)
      self.nodes.append(key)
    return self.table[key]

  def const(self, v, w):
    return self.node("const", None, (), w, v % (1 << w))

  def is_const(self, n, v=None):
    k = self.nodes[n]
    return k[0] == "const" and (v is None or k[4] == v % (1 << k[3]))

  def width(self, n):
    return self.nodes[n][3]

  def ones(self, w):
    return self.const(-1, w)

  def op(self, op, args, w, extra=None):
    """Create an op through the rewrite rules."""
    args = list(args)
    if op == "mux":
      return self.mux(args[0], args[1], args[2])  # no *args: C recursion limit
    if op in COMMUTATIVE:
      return self.assoc(op, args, w)
    return self.node("op", op, args, w, extra)

  def assoc(self, op, args, w):
    if not self.rules["assoc"]:
      return self.node("op", op, sorted(args), w)
    leaves = []
    stack = list(args)
    while stack:
      n = stack.pop()
      k = self.nodes[n]
      if k[0] == "op" and k[1] == op:
        stack.extend(k[2])
      else:
        leaves.append(n)
    if op == "xor":  # x ^ x = 0
      cnt = {}
      for l in leaves:
        cnt[l] = cnt.get(l, 0) ^ 1
      leaves = [l for l, c in cnt.items() if c]
    if op in IDENTITY:
      leaves = [l for l in leaves if not self.is_const(l, IDENTITY[op])]
    if op == "mul":
      leaves = [l for l in leaves if not self.is_const(l, 1)]
    if not leaves:
      return self.const(IDENTITY.get(op, 1), w)
    leaves.sort()
    acc = leaves[0]
    for l in leaves[1:]:
      acc = self.node("op", op, (acc, l), w)
    return acc

  def neg(self, n):
    return self.assoc("xor", [n, self.ones(self.width(n))], self.width(n))

  def is_neg(self, n):
    k = self.nodes[n]
    if k[0] == "op" and k[1] == "xor" and len(k[2]) == 2:
      a, b = k[2]
      if self.is_const(b, -1):
        return a
      if self.is_const(a, -1):
        return b
    return None

  def mux(self, s, x, y):
    key = (s, x, y)
    if key not in self.muxes:
      self.muxes[key] = self.mux_rules(s, x, y)
    return self.muxes[key]

  def mux_rules(self, s, x, y):
    w = self.width(x)
    if x == y:
      return x
    if self.is_const(s, 1):
      return x
    if self.is_const(s, 0):
      return y
    kx, ky = self.nodes[x], self.nodes[y]
    r = self.rules
    if r["not"]:
      nx, ny = self.is_neg(x), self.is_neg(y)
      if ny is not None and nx is None:
        ns = self.assoc("xor", [s, self.const(1, 1)], 1)
        rep = self.node("op", "replicate", (ns,), w)
        return self.assoc("xor", [self.mux(s, x, ny), rep], w)
      if nx is not None and ny is None:
        rep = self.node("op", "replicate", (s,), w)
        return self.assoc("xor", [self.mux(s, nx, y), rep], w)
    # Not through muxes: mux(s, mux(t,a,b), mux(u,c,d)) comes back to
    # itself after two push-downs (seen on CV32E40X ex_stage).
    if (r["push"] and kx[0] == ky[0] == "op" and kx[1] == ky[1] and
        kx[1] != "mux" and kx[4] == ky[4] and len(kx[2]) == len(ky[2]) and
        all(self.width(a) == self.width(b) for a, b in zip(kx[2], ky[2]))):
      args = [self.mux(s, a, b) for a, b in zip(kx[2], ky[2])]
      return self.op(kx[1], args, w, kx[4])
    if r["ident"]:
      for f, (big, other,
              flip) in ((kx[1] if kx[0] == "op" else None, (kx, y, False)),
                        (ky[1] if ky[0] == "op" else None, (ky, x, True))):
        if f in IDENTITY and len(
            big[2]) == 2 and not (self.nodes[other][0] == "op" and
                                  self.nodes[other][1] == f):
          a, b = big[2]
          e = self.const(IDENTITY[f], w)
          if flip:
            return self.assoc(f, [self.mux(s, other, a), self.mux(s, e, b)], w)
          return self.assoc(f, [self.mux(s, a, other), self.mux(s, b, e)], w)
    return self.node("op", "mux", (s, x, y), w)


LINE = re.compile(r"\s*%(\S+) = (\S+) (.*)$")


def width_of(ty):
  return 1 if ty.strip() == "i1" else int(ty.strip()[1:])


def normalize(line):
  """Reduce CIRCT's printed syntax to the subset parsed below (output of
    cv32e40x/lower_regs.py: attribute dicts, `bin` flags, register names and
    symbols, case equality on two-valued logic)."""
  line = re.sub(r" \{[^{}]*\}(?= :)", "", line)
  line = line.replace(" bin ", " ")
  line = re.sub(r'(seq\.compreg) (?:sym @\S+ )?(?:name "[^"]*" )?', r"\1 ",
                line)
  line = re.sub(r"comb\.icmp c(eq|ne) ", r"comb.icmp \1 ", line)
  return line.rstrip()


def parse_and_build(text, rules):
  lines = [normalize(l) for l in text.splitlines()]
  start = next(k for k, l in enumerate(lines) if "hw.module" in l)
  header = lines[start].strip()
  lines = lines[start:]
  ins = re.findall(r"in %([^\s:,]+) ?: (i\d+|!seq\.clock)", header)
  dag = Dag(rules)
  env = {}
  defs = {}
  regs = []
  output = None
  for l in lines[1:]:
    m = LINE.match(l)
    if m:
      defs[m.group(1)] = (m.group(2), m.group(3))
    elif l.strip().startswith("hw.output"):
      output = l.split()[1]
      break
  for name, ty in ins:
    if ty != "!seq.clock":
      env[name] = dag.node("in", None, (), width_of(ty), name)
  for name, (op, rest) in defs.items():
    if op == "seq.compreg":
      w = width_of(rest.split(":")[1])
      env[name] = dag.node("reg", None, (), w, name)
      regs.append((name, rest.split(",")[0].strip()[1:], w))

  def get(name):
    name = name.lstrip("%")
    if name in env:
      return env[name]
    op, rest = defs[name]
    if op == "hw.constant":
      parts = rest.split(":")
      v = parts[0].strip()
      if v in ("true", "false"):
        n = dag.const(1 if v == "true" else 0, 1)
      else:
        n = dag.const(int(v), width_of(parts[1]))
    elif op == "comb.replicate":
      arg = rest.split(":")[0].strip()
      w = width_of(rest.split("->")[1])
      n = dag.node("op", "replicate", (get(arg),), w)
    elif op == "comb.extract":
      arg, rest2 = rest.split(" from ")
      lo = int(rest2.split(":")[0])
      w = width_of(rest2.split("->")[1])
      n = dag.node("op", "extract", (get(arg.strip()),), w, lo)
    elif op == "comb.concat":
      body = rest.split(":")[0]
      args = [get(x.strip()) for x in body.split(",")]
      n = dag.node("op", "concat", args, sum(dag.width(a) for a in args))
    elif op == "comb.parity":
      arg = rest.split(":")[0].strip()
      n = dag.node("op", "parity", (get(arg),), 1)
    elif op == "comb.icmp":
      pred, args = rest.split(" ", 1)
      a, b = [x.strip() for x in args.split(":")[0].split(",")]
      n = dag.node("op", "icmp", (get(a), get(b)), 1, pred)
    else:
      body, ty = rest.rsplit(":", 1)
      args = [get(x.strip()) for x in body.split(",")]
      n = dag.op(op.split(".")[1], args, width_of(ty))
    env[name] = n
    return n

  nexts = [(r, get(nx), w) for r, nx, w in regs]
  bad = get(output)
  return header, dag, nexts, bad


def emit(header, dag, nexts, bad):
  out = [header]
  names = {}
  order = []
  seen = set()

  def visit(n):
    if n in seen:
      return
    seen.add(n)
    for a in dag.nodes[n][2]:
      visit(a)
    order.append(n)

  for _, nx, _ in nexts:
    visit(nx)
  visit(bad)
  for n in order:
    kind, op, args, w, extra = dag.nodes[n]
    if kind in ("in", "reg"):
      names[n] = "%" + extra
      continue
    nm = f"%n{n}"
    names[n] = nm
    if kind == "const":
      v = extra - (1 << w) if w > 1 and extra >> (w - 1) else extra
      out.append(f"  {nm} = hw.constant {v} : i{w}")
    elif op == "replicate":
      out.append(f"  {nm} = comb.replicate {names[args[0]]} : "
                 f"(i{dag.width(args[0])}) -> i{w}")
    elif op == "extract":
      out.append(f"  {nm} = comb.extract {names[args[0]]} from {extra}"
                 f" : (i{dag.width(args[0])}) -> i{w}")
    elif op == "concat":
      out.append(f"  {nm} = comb.concat " + ", ".join(names[a] for a in args) +
                 " : " + ", ".join(f"i{dag.width(a)}" for a in args))
    elif op == "parity":
      out.append(f"  {nm} = comb.parity {names[args[0]]} : "
                 f"i{dag.width(args[0])}")
    elif op == "icmp":
      out.append(f"  {nm} = comb.icmp {extra} {names[args[0]]}, "
                 f"{names[args[1]]} : i{dag.width(args[0])}")
    else:
      out.append(f"  {nm} = comb.{op} " + ", ".join(names[a] for a in args) +
                 f" : i{w}")
  for r, nx, w in nexts:
    out.append(f"  %{r} = seq.compreg {names[nx]}, %clk : i{w}")
  out.append(f"  hw.output {names[bad]} : i1")
  out.append("}")
  return "\n".join(out) + "\n"


def evaluate(dag, roots, env):
  """Values of roots given values of inputs and registers (by name)."""
  val = {}

  def ev(n):
    if n in val:
      return val[n]
    kind, op, args, w, extra = dag.nodes[n]
    m = (1 << w) - 1
    if kind in ("in", "reg"):
      v = env[extra]
    elif kind == "const":
      v = extra
    else:
      a = [ev(x) for x in args]
      if op == "add":
        v = sum(a)
      elif op == "mul":
        v = 1
        for x in a:
          v *= x
      elif op == "and":
        v = m
        for x in a:
          v &= x
      elif op == "or":
        v = 0
        for x in a:
          v |= x
      elif op == "xor":
        v = 0
        for x in a:
          v ^= x
      elif op == "sub":
        v = a[0] - a[1]
      elif op == "shl":
        v = a[0] << a[1] if a[1] < w else 0
      elif op == "shru":
        v = a[0] >> a[1]
      elif op == "extract":
        v = a[0] >> extra
      elif op == "concat":
        v = 0
        for x, n2 in zip(a, args):
          v = (v << dag.width(n2)) | x
      elif op == "parity":
        v = bin(a[0]).count("1") & 1
      elif op == "mux":
        v = a[1] if a[0] else a[2]
      elif op == "replicate":
        v = m if a[0] else 0
      elif op == "icmp":
        wa = dag.width(args[0])
        sg = [x - (1 << wa) if x >> (wa - 1) else x for x in a]
        if extra in ("slt", "sle", "sgt", "sge"):
          a = sg
          extra = {
              "slt": "ult",
              "sle": "ule",
              "sgt": "ugt",
              "sge": "uge"
          }[extra]
        v = int({
            "ne": a[0] != a[1],
            "eq": a[0] == a[1],
            "ugt": a[0] > a[1],
            "ult": a[0] < a[1],
            "uge": a[0] >= a[1],
            "ule": a[0] <= a[1]
        }[extra])
      else:
        raise SystemExit(f"cannot evaluate {op}")
    val[n] = v & m
    return val[n]

  return [ev(r) for r in roots]


def check(text, rules, trials=2000):
  """Random simulation of one step: next states and bad must agree."""
  import random
  off = {k: False for k in rules}
  h0, d0, n0, b0 = parse_and_build(text, off)
  h1, d1, n1, b1 = parse_and_build(text, rules)
  leaves = {k[4]: k[3] for k in d0.nodes if k[0] in ("in", "reg")}
  rng = random.Random(1)
  for _ in range(trials):
    env = {n: rng.getrandbits(w) for n, w in leaves.items()}
    r0 = evaluate(d0, [x for _, x, _ in n0] + [b0], env)
    r1 = evaluate(d1, [x for _, x, _ in n1] + [b1], env)
    if r0 != r1:
      raise SystemExit(f"mismatch on {env}: {r0} vs {r1}")


def main():
  rules = {"push": True, "ident": True, "not": True, "assoc": True}
  args = []
  do_check = False
  for a in sys.argv[1:]:
    if a.startswith("--no-"):
      rules[a[5:]] = False
    elif a == "--check":
      do_check = True
    else:
      args.append(a)
  if len(args) != 2:
    raise SystemExit(__doc__)
  text = open(args[0]).read()
  if do_check:
    check(text, rules)
  header, dag, nexts, bad = parse_and_build(text, rules)
  open(args[1], "w").write(emit(header, dag, nexts, bad))


if __name__ == "__main__":
  # The rewrites recurse along expression depth (thousands on real designs).
  import threading
  threading.stack_size(1 << 30)
  t = threading.Thread(target=main)
  t.start()
  t.join()
