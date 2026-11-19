# sizes.py: per-check table and per-design aggregates of the size study.
# Reads study32/raw/<design>.<check>.<cfg> ("and ff" after &scl, after
# &fraig, after &fraig;&scl) and writes study32/sizes.tsv and
# study32/sizes-summary.md (included into summary.md).
import os, statistics, sys

D = '/root/yjh-workspace/solver-oriented-out/study32'
CFGS = ['base', 'kbd', 'sc', 'ws3']


def raw(design, check, cfg):
    p = f'{D}/raw/{design}.{check}.{cfg}'
    v = list(map(int, open(p).read().split()))
    assert len(v) == 6, p
    return {'scl': (v[0], v[1]), 'fraig': (v[2], v[3]), 'fscl': (v[4], v[5])}


def checks(design):
    s = set()
    for f in os.listdir(f'{D}/raw'):
        d, rest = f.split('.', 1)
        if d == design:
            s.add(rest.rsplit('.', 1)[0])
    return sorted(s)


rows = []
for design in ['cv32', 'pico']:
    for c in checks(design):
        r = {cfg: raw(design, c, cfg) for cfg in CFGS}
        row = {'design': design, 'check': c}
        # (ands, latches) of each measured model.
        m = {
            'A0': r['base']['scl'],
            'A1': r['base']['fscl'],   # &scl; &fraig; &scl
            'A2': r['kbd']['fscl'],
            'A3': r['sc']['scl'],
            'A3F': r['sc']['fscl'],
            'W': r['ws3']['scl'],
            'WF': r['ws3']['fscl'],
        }
        for k, (a, f) in m.items():
            row[k + '_and'] = a
            row[k + '_ff'] = f
        for u in ['and', 'ff']:
            a0, a1, a2, a3, w = (row[k + '_' + u] for k in ['A0', 'A1', 'A2', 'A3', 'W'])
            row['sc_red_' + u] = a0 - a3
            row['comb_' + u] = a0 - a1
            row['const_' + u] = a1 - a2
            row['ind_' + u] = a2 - a3
            row['wl_red_' + u] = a0 - w
            row['wl_share_' + u] = (a0 - w) / (a0 - a3) if a0 != a3 else float('nan')
            row['wlf_red_' + u] = a0 - row['WF_' + u]
            row['wlf_share_' + u] = row['wlf_red_' + u] / (a0 - a3) if a0 != a3 else float('nan')
        rows.append(row)

cols = ['design', 'check'] + [f'{k}_{u}' for k in ['A0', 'A1', 'A2', 'A3', 'A3F', 'W', 'WF'] for u in ['and', 'ff']] + \
       [f'{k}_{u}' for u in ['and', 'ff'] for k in ['sc_red', 'comb', 'const', 'ind', 'wl_red', 'wl_share', 'wlf_red', 'wlf_share']]
with open(f'{D}/sizes.tsv', 'w') as f:
    f.write('\t'.join(cols) + '\n')
    for r in rows:
        f.write('\t'.join(f'{r[c]:.3f}' if isinstance(r[c], float) else str(r[c]) for c in cols) + '\n')


def pct(x):
    return f'{100 * x:.1f}%'


out = []
for design in ['cv32', 'pico']:
    R = [r for r in rows if r['design'] == design]
    out.append(f'### {design} ({len(R)} checks)\n')
    out.append('Sums and medians over the checks (ANDs / latches after `&scl`):\n')
    out.append('| model | sum ANDs | median ANDs | sum latches | median latches |')
    out.append('|---|---|---|---|---|')
    names = {'A0': 'A0 base', 'A1': 'A1 base+&fraig', 'A2': 'A2 known bits+&fraig', 'A3': 'A3 sc',
             'A3F': 'sc+&fraig', 'W': 'W ws3', 'WF': 'WF ws3+&fraig'}
    for k, n in names.items():
        a = [r[k + '_and'] for r in R]; f = [r[k + '_ff'] for r in R]
        out.append(f'| {n} | {sum(a)} | {statistics.median(a):g} | {sum(f)} | {statistics.median(f):g} |')
    out.append('')
    out.append("Split of sc's reduction A0-A3 (share of the summed reduction; median of the per-check shares over checks with A0>A3):\n")
    out.append('| part | ANDs sum | share of sum | median share | latches sum | share of sum | median share |')
    out.append('|---|---|---|---|---|---|---|')
    for k, n in [('sc_red', 'sc total A0-A3'), ('comb', 'combinational A0-A1'), ('const', 'constant register bits A1-A2'),
                 ('ind', 'needs induction A2-A3'), ('wl_red', 'WL without SAT A0-W'),
                 ('wlf_red', 'WL then &fraig A0-WF')]:
        cells = []
        for u in ['and', 'ff']:
            tot = sum(r['sc_red_' + u] for r in R)
            s = sum(r[k + '_' + u] for r in R)
            sh = [r[k + '_' + u] / r['sc_red_' + u] for r in R if r['sc_red_' + u] > 0]
            cells += [str(s), pct(s / tot) if tot else '-', pct(statistics.median(sh)) if sh else '-']
        out.append(f'| {n} | ' + ' | '.join(cells) + ' |')
    out.append('')
    for u, un in [('and', 'ANDs'), ('ff', 'latches')]:
        neg = sum(1 for r in R if r['ind_' + u] < 0)
        nsc = sum(1 for r in R if r['sc_red_' + u] <= 0)
        wl_gt = sum(1 for r in R if r['wl_red_' + u] > r['sc_red_' + u])
        out.append(f'- {un}: induction remainder A2-A3 negative on {neg} of {len(R)} checks; '
                   f'sc reduction <= 0 on {nsc}; WL reduces more than sc on {wl_gt}.')
    out.append('')
open(f'{D}/sizes-summary.md', 'w').write('\n'.join(out) + '\n')
print('\n'.join(out))
