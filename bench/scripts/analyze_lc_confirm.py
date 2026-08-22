#!/usr/bin/env python3
"""Analyze LC confirmation sweep (multiple seeds per combo).

Reads bench/results/lc/confirm/*.json. Normalizes each (case, corpus, seed)
series by its own best combo, then prints per-combo geomean.
"""
import glob
import json
import math
import os
import sys

def metric(c):
    if c.get("amortized_ns", 0) > 0:
        return c["amortized_ns"]
    return c["ns_per_op"]

def main():
    pattern = sys.argv[1] if len(sys.argv) > 1 else "bench/results/lc/confirm/lc_f*_lf*_seed*.json"
    files = sorted(glob.glob(pattern))
    if not files:
        print("no LC confirm JSONs found", file=sys.stderr)
        return 1

    series = {}
    combos = set()
    for path in files:
        try:
            with open(path) as f:
                d = json.load(f)
        except (ValueError, OSError):
            continue
        p = d.get("params", {})
        if "LC_FANOUT" not in p or "LC_LEAF_FANOUT" not in p:
            continue
        combo = (p["LC_FANOUT"], p["LC_LEAF_FANOUT"])
        seed = p.get("seed", 0)
        combos.add(combo)
        for c in d.get("cases", []):
            key = (c["name"], c["corpus"], seed)
            series.setdefault(key, {})[combo] = metric(c)
    combos = sorted(combos)
    keys = sorted(series.keys())
    geomean = {}
    for combo in combos:
        logs = []
        for key in keys:
            vals = series[key]
            if combo not in vals:
                continue
            best = min(vals.values())
            if best <= 0:
                continue
            logs.append(math.log(vals[combo] / best))
        if logs:
            geomean[combo] = math.exp(sum(logs) / len(logs))

    for combo in combos:
        g = geomean.get(combo)
        print("LC_FANOUT=%-3d LC_LEAF_FANOUT=%-3d geomean=%.4f" %
              (combo[0], combo[1], g if g is not None else float("nan")))

    best = min(geomean, key=geomean.get)
    print("\nbest combo: LC_FANOUT=%d LC_LEAF_FANOUT=%d geomean=%.4f" %
          (best[0], best[1], geomean[best]))
    print("series: %d, combos: %d" % (len(keys), len(combos)))
    return 0

if __name__ == "__main__":
    sys.exit(main())
