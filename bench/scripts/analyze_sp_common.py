#!/usr/bin/env python3
"""Analyze full SP_FANOUT sweep using only cases present in every JSON."""
import glob
import json
import math
import os
import sys


def metric(c):
    if c.get("amortized_ns") and c["amortized_ns"] > 0:
        return c["amortized_ns"]
    return c["ns_per_op"]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    pattern = os.path.join(os.path.dirname(here), "results", "sp", "sp_fanout_*.json")
    files = sorted(glob.glob(os.path.normpath(pattern)))
    data = {}
    for p in files:
        try:
            with open(p, "r", encoding="utf-8") as fh:
                d = json.load(fh)
        except (json.JSONDecodeError, OSError):
            continue
        f = d.get("params", {}).get("SP_FANOUT")
        if f is None:
            continue
        data[f] = {(c["name"], c["corpus"]): metric(c) for c in d["cases"]}
    fanouts = sorted(data)
    common = set.intersection(*(set(d) for d in data.values())) if data else set()
    if not common:
        print("no common cases", file=sys.stderr)
        return 1

    best = {c: min(data[f][c] for f in fanouts) for c in common}
    print("fanouts", len(fanouts), "common cases", len(common))
    print("\nnormalized geomean (lower=better)")
    print("%4s %10s" % ("fan", "geomean"))
    geos = {}
    for f in fanouts:
        ratios = [data[f][c] / best[c] for c in common]
        g = math.exp(sum(math.log(r) for r in ratios) / len(ratios))
        geos[f] = g
        print("%4d %10.4f" % (f, g))
    bf = min(geos, key=lambda f: geos[f])
    print("\nbest fanout: %d (%.4f)" % (bf, geos[bf]))
    print("top 10:")
    for f, g in sorted(geos.items(), key=lambda x: x[1])[:10]:
        print("  %3d %.4f" % (f, g))
    return 0


if __name__ == "__main__":
    sys.exit(main())
