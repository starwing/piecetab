#!/usr/bin/env python3
"""Analyze LC_FANOUT x LC_LEAF_FANOUT coarse probe JSONs.

Prints a normalized-geomean matrix over all (case, corpus) measurements.
Lower is better; each case series is normalized by its own best combo.
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
    pattern = sys.argv[1] if len(sys.argv) > 1 else "bench/results/lc/probe/lc_f*_lf*.json"
    files = sorted(glob.glob(pattern))
    if not files:
        print("no probe JSONs found", file=sys.stderr)
        return 1

    combos = []
    series = {}
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
        combos.append(combo)
        for c in d.get("cases", []):
            key = (c["name"], c["corpus"])
            series.setdefault(key, {})[combo] = metric(c)
    if not combos:
        print("no LC params found", file=sys.stderr)
        return 1

    combos = sorted(set(combos))
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

    fanouts = sorted({f for f, _ in combos})
    leafs = sorted({lf for _, lf in combos})
    print("LC_LEAF_FANOUT -> " + " ".join("%8d" % lf for lf in leafs))
    for f in fanouts:
        row = []
        for lf in leafs:
            g = geomean.get((f, lf))
            row.append("%8.3f" % g if g is not None else "     -  ")
        print("LC_FANOUT %-3d  %s" % (f, " ".join(row)))

    best = min(geomean, key=geomean.get)
    print("\nbest combo: LC_FANOUT=%d LC_LEAF_FANOUT=%d geomean=%.3f" %
          (best[0], best[1], geomean[best]))
    print("combos measured: %d, case series: %d" % (len(combos), len(keys)))
    return 0

if __name__ == "__main__":
    sys.exit(main())
