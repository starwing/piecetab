#!/usr/bin/env python3
import json
import math
import os
import sys

def load(path):
    with open(path) as f:
        return json.load(f)

def geom(xs):
    if not xs:
        return 0.0
    return math.exp(sum(math.log(x) for x in xs) / len(xs))

def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'bench/results/sp/sparse_confirm'
    files = sorted(
        os.path.join(root, f)
        for f in os.listdir(root)
        if f.startswith('sp_fanout_') and f.endswith('.json')
    )
    data = {}
    for path in files:
        d = load(path)
        fan = d['params']['SP_FANOUT']
        data[fan] = {c['name'] + '_' + c['corpus']: c['ns_per_op'] for c in d['cases']}

    cases = sorted({k for d in data.values() for k in d})
    best = {c: min(d[c] for d in data.values() if c in d) for c in cases}

    print('%-42s' % 'case', end='')
    for fan in sorted(data):
        print('%10d' % fan, end='')
    print()

    patterns = {
        'all': cases,
        'fragmented_sparse': [c for c in cases if 'fragmented_sparse' in c],
        'viewport': [c for c in cases if 'viewport' in c],
        'scattered': [c for c in cases if 'scattered' in c],
    }

    for label, cs in patterns.items():
        print('\n[%s] normalized geomean (lower=better)' % label)
        print('%-42s' % 'group', end='')
        for fan in sorted(data):
            print('%10.4f' % geom([data[fan][c] / best[c] for c in cs if c in data[fan]]), end='')
        print()

    print('\nper-case best fanout:')
    for c in cases:
        vals = [(fan, data[fan][c]) for fan in sorted(data) if c in data[fan]]
        fan, val = min(vals, key=lambda x: x[1])
        print('%-42s fanout=%2d  %12.3f' % (c, fan, val))

if __name__ == '__main__':
    main()
