#!/usr/bin/env python3
"""Analyze one-dimensional LC sweeps (fanout or leaf).

Reads bench/results/lc/fanout/*.json or bench/results/lc/leaf/*.json.
Prints per-case best and normalized geomean for the swept parameter.
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
    pattern = sys.argv[1] if len(sys.argv) > 1 else "bench/results/lc/fanout/lc_f*_lf*.json"
    files = sorted(glob.glob(pattern))
    if not files:
        print("no LC sweep JSONs found", file=sys.stderr)
        return 1

    series = {}
    params = []
    for path in files:
        try:
            with open(path) as f:
                d = json.load(f)
        except (ValueError, OSError):
            continue
        p = d.get("params", {})
        if "LC_FANOUT" not in p or "LC_LEAF_FANOUT" not in p:
            continue
        params.append((p["LC_FANOUT"], p["LC_LEAF_FANOUT"]))
        for c in d.get("cases", []):
            key = (c["name"], c["corpus"])
            series.setdefault(key, {})[(p["LC_FANOUT"], p["LC_LEAF_FANOUT"])] = metric(c)
    combos = sorted(set(params))
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

    # Determine which parameter varies.
    fanouts = {f for f, _ in combos}
    leafs = {lf for _, lf in combos}
    if len(fanouts) > 1 and len(leafs) == 1:
        header = "LC_FANOUT"
        fixed_lf = leafs.pop()
        fixed = "LC_LEAF_FANOUT=%d" % fixed_lf
        values = sorted(fanouts)
        lookup = lambda v: geomean.get((v, fixed_lf))
    elif len(leafs) > 1 and len(fanouts) == 1:
        header = "LC_LEAF_FANOUT"
        fixed_f = fanouts.pop()
        fixed = "LC_FANOUT=%d" % fixed_f
        values = sorted(leafs)
        lookup = lambda v: geomean.get((fixed_f, v))
    else:
        header = "combo"
        fixed = ""
        values = combos
        lookup = geomean.get
        header = "combo"
        fixed = ""
        values = combos

    print("fixed: %s" % fixed if fixed else "2D sweep")
    for v in values:
        g = lookup(v)
        print("%s %-4s geomean=%.4f" % (header, str(v), g if g is not None else float("nan")))

    best = min(geomean, key=geomean.get)
    print("\nbest %s=%s geomean=%.4f" % (header, str(best), geomean[best]))
    print("series: %d, combos: %d" % (len(keys), len(combos)))
    return 0

if __name__ == "__main__":
    sys.exit(main())
