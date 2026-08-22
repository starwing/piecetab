#!/usr/bin/env python3
"""Analyze SP_FANOUT sweep JSONs: per-case best and normalized geomean."""
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
    if not files:
        print("no sp_fanout JSON files", file=sys.stderr)
        return 1
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
        data[f] = d["cases"]
    fanouts = sorted(data)
    if not fanouts:
        print("no SP_FANOUT params found", file=sys.stderr)
        return 1
    cases = set()
    for f in fanouts:
        for c in data[f]:
            cases.add((c["name"], c["corpus"]))
    print("fanouts", fanouts)
    print("cases", len(cases))
    print("\nper-case best:")
    print("%-12s %-16s %6s %10s" % ("case", "corpus", "fanout", "metric"))
    for key in sorted(cases):
        vals = [(f, metric(c)) for f in fanouts
                for c in data[f] if (c["name"], c["corpus"]) == key]
        best = min(vals, key=lambda x: x[1])
        print("%-12s %-16s %6d %10.3f" % (key[0], key[1], best[0], best[1]))
    geos = {}
    for f in fanouts:
        ratios = []
        for key in cases:
            for c in data[f]:
                if (c["name"], c["corpus"]) == key:
                    vals = [metric(x) for ff in fanouts for x in data[ff]
                            if (x["name"], x["corpus"]) == key]
                    base = min(vals)
                    ratios.append(metric(c) / base)
                    break
        geos[f] = math.exp(sum(math.log(r) for r in ratios) / len(ratios))
    print("\nfanout geomean")
    for f in fanouts:
        print("%3d %.4f" % (f, geos[f]))
    best = min(geos, key=lambda f: geos[f])
    print("best", best, geos[best])
    return 0


if __name__ == "__main__":
    sys.exit(main())
