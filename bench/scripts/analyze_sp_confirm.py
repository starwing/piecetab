#!/usr/bin/env python3
"""Analyze SP_FANOUT confirmation JSONs and print normalized geomeans."""
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
    pattern = os.path.join(
        os.path.dirname(here), "results", "sp", "confirm",
        "sp_fanout_*_seed*.json")
    files = sorted(glob.glob(os.path.normpath(pattern)))
    if not files:
        print("no sp confirm files", file=sys.stderr)
        return 1
    data = {}
    for p in files:
        base = os.path.basename(p)
        body = base.replace("sp_fanout_", "").replace(".json", "")
        parts = body.split("_seed")
        if len(parts) != 2:
            continue
        f = int(parts[0])
        s = int(parts[1])
        try:
            with open(p, "r", encoding="utf-8") as fh:
                d = json.load(fh)
        except (json.JSONDecodeError, OSError):
            continue
        data.setdefault(f, {})[s] = {}
        for c in d["cases"]:
            data[f][s][(c["name"], c["corpus"])] = metric(c)
    fanouts = sorted(data)
    if not fanouts:
        print("no SP confirm data", file=sys.stderr)
        return 1
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
                vals = [data[ff][s][c] for ff in fanouts
                        if s in data[ff] and c in data[ff][s]]
                if not vals:
                    continue
                base = min(vals)
                ratios.append(data[f][s][c] / base)
        geos[f] = math.exp(sum(math.log(r) for r in ratios) / len(ratios))
    print("fanout geomean")
    for f in fanouts:
        print("%3d %.4f" % (f, geos[f]))
    best = min(geos, key=lambda f: geos[f])
    print("best", best, geos[best])
    return 0


if __name__ == "__main__":
    sys.exit(main())
