#!/usr/bin/env python3
"""Read bench JSON files and produce FANOUT curves or a CSV fallback.

Usage:
  python3 bench/scripts/plot_bench.py bench/results/pt_fanout_*.json --out bench/reports
"""

import argparse
import csv
import glob
import json
import os
import sys

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    HAVE_MPL = True
except Exception:
    HAVE_MPL = False


def load_files(paths):
    rows = []
    for path in paths:
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        fanout = data.get("params", {}).get("PT_FANOUT")
        for case in data.get("cases", []):
            case["fanout"] = fanout
            case["source"] = path
            rows.append(case)
    return rows


def metric(r):
    """Use amortized ns/op for full-scan cases, raw ns/op otherwise."""
    a = r.get("amortized_ns")
    if a:
        return a
    return r.get("ns_per_op", 0.0)


def best_by_case(rows):
    grouped = {}
    for r in rows:
        key = (r.get("name"), r.get("corpus"))
        grouped.setdefault(key, []).append(r)
    best = []
    for key, items in grouped.items():
        items.sort(key=metric)
        top = items[0]
        best.append(
            {
                "case": key[0],
                "corpus": key[1],
                "fanout": top.get("fanout"),
                "ns_per_op": top.get("ns_per_op"),
                "amortized_ns": top.get("amortized_ns", 0.0),
                "metric_ns": metric(top),
            }
        )
    best.sort(key=lambda x: (x["case"], x["corpus"]))
    return best


def write_csv(rows, path):
    with open(path, "w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(
            ["case", "corpus", "fanout", "ns_per_op", "amortized_ns", "calls",
             "metric_ns", "median_ns", "min_ns", "p10_ns", "p90_ns", "iters",
             "rounds"]
        )
        for r in sorted(
            rows,
            key=lambda x: (
                x.get("name", ""),
                x.get("corpus", ""),
                x.get("fanout") or 0,
            ),
        ):
            writer.writerow(
                [
                    r.get("name"),
                    r.get("corpus"),
                    r.get("fanout"),
                    r.get("ns_per_op"),
                    r.get("amortized_ns", 0.0),
                    r.get("calls", 0),
                    metric(r),
                    r.get("median_ns"),
                    r.get("min_ns"),
                    r.get("p10_ns"),
                    r.get("p90_ns"),
                    r.get("iters"),
                    r.get("rounds"),
                ]
            )


def write_best_md(best, path, meta):
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("# PT_FANOUT sweep: best per case\n\n")
        fh.write("machine: %s\n\n" % meta.get("machine", "unknown"))
        fh.write("Note: full-scan cases (`pt_next`/`pt_prev`) use `amortized_ns` "
                 "(per successful call), not the whole-traversal time.\n\n")
        fh.write("| case | corpus | best FANOUT | metric ns/op |\n")
        fh.write("|---|---|---|---|\n")
        for b in best:
            fh.write(
                "| %s | %s | %s | %.3f |\n"
                % (b["case"], b["corpus"], b["fanout"], b["metric_ns"])
            )


def make_plots(rows, outdir):
    grouped = {}
    for r in rows:
        key = (r.get("name"), r.get("corpus"))
        grouped.setdefault(key, []).append(r)

    for key, items in grouped.items():
        items.sort(key=lambda x: x.get("fanout") or 0)
        xs = [x.get("fanout") for x in items]
        ys = [metric(x) for x in items]
        fig, ax = plt.subplots(figsize=(7, 4))
        ax.plot(xs, ys, marker="o", label=key[1])
        best_i = min(range(len(ys)), key=lambda i: ys[i])
        ax.plot(xs[best_i], ys[best_i], "r*", ms=14,
                label="best FANOUT=%s" % xs[best_i])
        ax.set_xlabel("PT_FANOUT")
        ax.set_ylabel("metric ns/op")
        ax.set_title("%s (%s)" % key)
        ax.legend()
        ax.grid(True, alpha=0.3)
        fig.tight_layout()
        base = "%s_%s" % (key[0], key[1].replace("/", "_"))
        fig.savefig(os.path.join(outdir, base + ".png"), dpi=150)
        fig.savefig(os.path.join(outdir, base + ".svg"))
        plt.close(fig)

    # overview: one subplot per case, all corpora in that case
    by_case = {}
    for r in rows:
        by_case.setdefault(r.get("name"), []).append(r)
    n = len(by_case)
    if n:
        cols = 4
        rows_n = (n + cols - 1) // cols
        fig, axes = plt.subplots(rows_n, cols, figsize=(16, 3.5 * rows_n),
                                 squeeze=False)
        for idx, (case_name, items) in enumerate(sorted(by_case.items())):
            ax = axes[idx // cols][idx % cols]
            by_corpus = {}
            for r in items:
                by_corpus.setdefault(r.get("corpus"), []).append(r)
            for corpus, citems in by_corpus.items():
                citems.sort(key=lambda x: x.get("fanout") or 0)
                xs = [x.get("fanout") for x in citems]
                ys = [metric(x) for x in citems]
                ax.plot(xs, ys, marker="o", label=corpus)
            ax.set_title(case_name, fontsize=9)
            ax.set_xlabel("PT_FANOUT", fontsize=8)
            ax.set_ylabel("metric ns/op", fontsize=8)
            ax.grid(True, alpha=0.3)
            ax.legend(fontsize=6)
        for idx in range(n, rows_n * cols):
            axes[idx // cols][idx % cols].axis("off")
        fig.tight_layout()
        fig.savefig(os.path.join(outdir, "overview.png"), dpi=150)
        fig.savefig(os.path.join(outdir, "overview.svg"))
        plt.close(fig)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_out = os.path.join(os.path.dirname(here), "reports")
    ap = argparse.ArgumentParser()
    ap.add_argument("jsons", nargs="+")
    ap.add_argument("--out", default=default_out)
    args = ap.parse_args()

    paths = []
    for pat in args.jsons:
        paths.extend(glob.glob(pat))
    if not paths:
        print("no JSON files matched", file=sys.stderr)
        return 1
    paths = sorted(set(paths))
    rows = load_files(paths)
    if not rows:
        print("no cases found in JSON files", file=sys.stderr)
        return 1

    os.makedirs(args.out, exist_ok=True)
    meta = {}
    with open(paths[0], "r", encoding="utf-8") as fh:
        meta = json.load(fh)

    best = best_by_case(rows)
    write_csv(rows, os.path.join(args.out, "pt_fanout_curves.csv"))
    write_best_md(best, os.path.join(args.out, "pt_fanout_best.md"), meta)
    print("wrote %s and %s" % (
        os.path.join(args.out, "pt_fanout_curves.csv"),
        os.path.join(args.out, "pt_fanout_best.md")))

    if HAVE_MPL:
        make_plots(rows, args.out)
        print("wrote PNG/SVG plots to %s" % args.out)
    else:
        print("matplotlib not installed; skipped plots (CSV/MD written)",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
