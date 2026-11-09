#!/usr/bin/env python3
"""Fuzz synth-remove-false-cycles on random 1-bit cyclic AIGs.

    fuzz_remove_false_cycles.py CIRCT-OPT [SEED] [COUNT]

For each random circuit (inputs a0..a2, nodes n0..nK, output = all nodes),
the fixed points of the original are enumerated for every input value.
- plain mode: if the pass rebuilt the cycle, every input must give exactly
  one fixed point and the rebuilt outputs must equal it.
- fixpoint-obligation: obligation == 0 iff exactly one fixed point; when 0,
  the outputs must equal it.
- unroll obligation (k=1, 3): when obligation == 0 for all Y1, Y2, the
  outputs must equal the unique fixed point.
- with either obligation, it must be 1 (for some Y1, Y2) where the module
  does not have exactly one fixed point.
"""
import itertools, random, re, subprocess, sys

OPT = sys.argv[1]
N_IN, rng = 3, random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 0)


def gen(nodes):
  names = [f"a{i}" for i in range(N_IN)] + [f"n{j}" for j in range(nodes)]
  ops = []
  for j in range(nodes):
    k = rng.randint(1, 3)
    ins = [(rng.choice(names), rng.random() < 0.5) for _ in range(k)]
    ops.append(ins)
  return ops


def to_mlir(ops):
  ins = ", ".join(f"in %a{i} : i1" for i in range(N_IN))
  outs = ", ".join(f"out o{j} : i1" for j in range(len(ops)))
  body = []
  for j, op in enumerate(ops):
    args = ", ".join(("not " if inv else "") + "%" + n for n, inv in op)
    body.append(f"  %n{j} = synth.aig.and_inv {args} : i1")
  body.append("  hw.output " + ", ".join(f"%n{j}" for j in range(len(ops))) +
              " : " + ", ".join("i1" for _ in ops))
  return f"hw.module @T({ins}, {outs}) {{\n" + "\n".join(body) + "\n}\n"


def fixed_points(ops, inputs):
  res = []
  for vals in itertools.product([0, 1], repeat=len(ops)):
    env = dict(inputs)
    env.update({f"n{j}": v for j, v in enumerate(vals)})
    if all(vals[j] == int(all(env[n] ^ inv
                              for n, inv in op))
           for j, op in enumerate(ops)):
      res.append(vals)
  return res


LINE = re.compile(r"%(\S+) = (synth\.aig\.and_inv|hw\.constant|comb\.extract|"
                  r"comb\.concat) (.*?) :")


def parse(text):
  """Returns (inputs, outputs, defs) of the transformed module."""
  head = re.search(r"hw\.module @T\((.*?)\) \{", text).group(1)
  ins = re.findall(r"in %(\S+) : i(\d+)", head)
  outs = re.findall(r"out (\S+) : i1", head)
  defs = {}
  for m in LINE.finditer(text):
    defs[m.group(1)] = (m.group(2), m.group(3))
  for m in re.finditer(r"%(\S+) = hw\.constant (true|false)\b", text):
    defs[m.group(1)] = ("hw.constant", m.group(2))
  ret = re.search(r"hw\.output (.*?) :", text).group(1)
  return ins, outs, [r.strip()[1:] for r in ret.split(",")], defs


def evaluate(defs, env, name, depth=0):
  if name in env:
    return env[name]
  if depth > 10000:
    raise RuntimeError("cycle left")
  kind, args = defs[name]
  if kind == "hw.constant":
    v = 1 if args.strip() == "true" else 0
  elif kind == "comb.extract":
    src, low = re.match(r"%(\S+) from (\d+)", args).groups()
    v = (evaluate(defs, env, src, depth + 1) >> int(low)) & 1
  elif kind == "comb.concat":
    v = 0
    for a in args.split(","):
      v = (v << 1) | evaluate(defs, env, a.strip()[1:], depth + 1)
  else:
    v = 1
    for a in args.split(","):
      a = a.strip()
      inv = a.startswith("not ")
      bit = evaluate(defs, env, a[4:][1:] if inv else a[1:], depth + 1)
      v &= bit ^ inv
  env[name] = v
  return v


def run(mlir, opts):
  p = subprocess.run([
      OPT, "-", "--pass-pipeline=builtin.module(hw.module("
      f"synth-remove-false-cycles{{{opts}}}))"
  ],
                     input=mlir,
                     capture_output=True,
                     text=True)
  assert p.returncode == 0, p.stderr
  return p.stdout


def check(ops, opts):
  text = run(to_mlir(ops), opts)
  ins, outs, rets, defs = parse(text)
  extra = [(n, int(w)) for n, w in ins if not n.startswith("a")]
  n = len(ops)
  for inp in itertools.product([0, 1], repeat=N_IN):
    env0 = {f"a{i}": inp[i] for i in range(N_IN)}
    fps = fixed_points(ops, env0)
    ys = [
        dict(zip([e
                  for e, _ in extra], vs))
        for vs in itertools.product(*[range(1 << w) for _, w in extra])
    ] or [{}]
    results = []
    for y in ys:
      env = dict(env0)
      env.update(y)
      try:
        results.append([evaluate(defs, env, r) for r in rets])
      except RuntimeError:
        return "cycle-left"  # the pass left the cycle: nothing to check
    if len(rets) == n:  # no obligation output
      vals = results[0]
      if len(fps) != 1 or tuple(vals) != fps[0]:
        return f"WRONG plain {inp} fps={fps} got={vals}"
      continue
    # The obligation holds at this input if it is 0 for all Y1, Y2.
    if all(not vals[-1] for vals in results):
      for vals in results:
        if len(fps) != 1 or tuple(vals[:-1]) != fps[0]:
          return f"WRONG obligation=0 {inp} fps={fps} got={vals}"
    # Obligations are per cycle, so they may be set although the module as
    # a whole has one fixed point; but they must be set if it has not.
    if len(fps) != 1 and not any(vals[-1] for vals in results):
      return f"WRONG obligation not set {inp} fps={fps}"
  return "ok"


def main():
  counts = {}
  for t in range(int(sys.argv[3]) if len(sys.argv) > 3 else 300):
    ops = gen(rng.randint(1, 4))
    for opts in [
        "", "obligation fixpoint-obligation", "obligation obligation-unroll=1",
        "obligation obligation-unroll=3"
    ]:
      r = check(ops, opts)
      counts[(opts, r.split(" ")[0])] = counts.get(
          (opts, r.split(" ")[0]), 0) + 1
      if r.startswith("WRONG"):
        print(to_mlir(ops), opts, r)
        sys.exit(1)
  for k, v in sorted(counts.items()):
    print(k, v)


main()
