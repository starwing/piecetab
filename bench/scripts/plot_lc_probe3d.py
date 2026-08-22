#!/usr/bin/env python3
"""Plot LC_FANOUT x LC_LEAF_FANOUT coarse probe as a 3D surface + heatmap.

Reads probe JSONs from bench/results/lc/probe/ and writes:
  bench/reports/lc/probe_3d.png
  bench/reports/lc/probe_3d.svg

The z value is the normalized geomean over all (case, corpus) series.
Lower is better; each series is normalized by its own best combo.
"""
import glob
import json
import math
import os
import sys

os.environ.setdefault("MPLCONFIGDIR", "/tmp/mpl")
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from mpl_toolkits.mplot3d import Axes3D  # noqa: F401


def metric(c):
    if c.get("amortized_ns", 0) > 0:
        return c["amortized_ns"]
    return c["ns_per_op"]


def load_geomean(pattern):
    files = sorted(glob.glob(pattern))
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
        raise SystemExit("no LC probe JSONs found for %s" % pattern)

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
    z = np.full((len(fanouts), len(leafs)), np.nan)
    for (f, lf), g in geomean.items():
        z[fanouts.index(f), leafs.index(lf)] = g
    return fanouts, leafs, z, geomean


def main():
    pattern = sys.argv[1] if len(sys.argv) > 1 else \
        "bench/results/lc/probe/lc_f*_lf*.json"
    out_prefix = sys.argv[2] if len(sys.argv) > 2 else "bench/reports/lc/probe_3d"
    fanouts, leafs, z, geomean = load_geomean(pattern)

    os.makedirs(os.path.dirname(out_prefix), exist_ok=True)
    x, y = np.meshgrid(fanouts, leafs, indexing="ij")
    fig = plt.figure(figsize=(13, 5.2))
    fig.suptitle("linecache coarse probe: normalized geomean (lower is better)")

    ax = fig.add_subplot(1, 2, 1, projection="3d")
    surf = ax.plot_surface(x, y, z, cmap="viridis", edgecolor="k",
                           linewidth=0.3, alpha=0.9)
    ax.view_init(elev=25, azim=135)
    ax.set_xlabel("LC_FANOUT")
    ax.set_ylabel("LC_LEAF_FANOUT")
    ax.set_zlabel("geomean")
    ax.set_title("3D surface")
    fig.colorbar(surf, ax=ax, shrink=0.6, aspect=12)

    ax2 = fig.add_subplot(1, 2, 2)
    im = ax2.imshow(z, origin="lower", aspect="auto", cmap="viridis")
    ax2.set_xticks(range(len(leafs)))
    ax2.set_xticklabels(leafs)
    ax2.set_yticks(range(len(fanouts)))
    ax2.set_yticklabels(fanouts)
    ax2.set_xlabel("LC_LEAF_FANOUT")
    ax2.set_ylabel("LC_FANOUT")
    ax2.set_title("heatmap")
    for i in range(len(fanouts)):
        for j in range(len(leafs)):
            if not np.isnan(z[i, j]):
                ax2.text(j, i, "%.3f" % z[i, j], ha="center", va="center",
                         fontsize=7, color="white")
    fig.colorbar(im, ax=ax2, shrink=0.8)

    best = min(geomean, key=geomean.get)
    fig.text(0.5, 0.01,
             "best: LC_FANOUT=%d LC_LEAF_FANOUT=%d geomean=%.3f" %
             (best[0], best[1], geomean[best]), ha="center", fontsize=10)

    fig.tight_layout(rect=(0, 0.04, 1, 0.95))
    for ext in ("png", "svg"):
        fig.savefig(out_prefix + "." + ext, dpi=160)
    print("wrote %s.png / %s.svg" % (out_prefix, out_prefix))
    return 0


if __name__ == "__main__":
    sys.exit(main())
