#!/usr/bin/env python3
"""Analyze bench/results/confirm JSONs and print normalized geomeans per FANOUT."""
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
    pattern = os.path.join(os.path.dirname(__file__), "..", "results", "confirm", "pt_fanout_*_seed*.json")
    files = sorted(glob.glob(os.path.normpath(pattern)))
    if not files:
        print("no confirm files", file=sys.stderr)
        return 1
    data = {}
    for p in files:
        base = os.path.basename(p)
        body = base.replace("pt_fanout_", "").replace(".json", "")
        parts = body.split("_seed")
        f = int(parts[0])
        s = int(parts[1])
        with open(p) as fh:
            d = json.load(fh)
        data.setdefault(f, {})[s] = {}
        for c in d["cases"]:
            data[f][s][(c["name"], c["corpus"])] = metric(c)
    fanouts = sorted(data)
    cases = set()
    for f in fanouts:
        for s in data[f]:
            cases.update(data[f][s].keys())
    print("fanouts", fanouts)
    print("cases", len(cases))
    geos = {}
    for f in fanouts:
        ratios = []
        for c in sorted(cases):
            for s in sorted(data[f]):
                vals = [data[ff][s][c] for ff in fanouts if s in data[ff] and c in data[ff][s]]
                base = min(vals)
                ratios.append(data[f][s][c] / base)
        geos[f] = math.exp(sum(math.log(r) for r in ratios) / len(ratios))
    print("fanout geomean")
    for f in fanouts:
        print("%3d %.4f" % (f, geos[f]))
    best = min(geos, key=lambda f: geos[f])
    print("best", best, geos[best])
    cand = [f for f in fanouts if f <= 32 and geos[f] <= geos[best] * 1.02]
    print("within 2% <=32", cand)
    return 0

if __name__ == "__main__":
    sys.exit(main())
