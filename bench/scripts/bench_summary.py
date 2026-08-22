#!/usr/bin/env python3
"""Print a compact benchmark summary table for the current tuned defaults.

Reads the default-fanout result JSONs and prints a Markdown table with
representative ns/op values on the 100k corpus.  Operations are rows and
libraries are columns:

- piecetab.h  : bench/results/pt/pt_fanout_31.json        (PT_FANOUT=31)
- linecache.h : bench/results/lc/confirm/lc_f16_lf34_seed1.json (LC_FANOUT=16, LC_LEAF_FANOUT=34)
- spantree.h  : bench/results/sp/sp_fanout_34.json        (SP_FANOUT=34)

Common rows: seek, locate, advance, splice.
Next row is per-item amortized for piecetab/spantree.
Unique rows: pt_edit / lc_scan (per line) / sp_fill.
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

FILES = {
    "piecetab.h": os.path.join(ROOT, "bench/results/pt/pt_fanout_31.json"),
    "linecache.h": os.path.join(ROOT, "bench/results/lc/confirm/lc_f16_lf34_seed1.json"),
    "spantree.h": os.path.join(ROOT, "bench/results/sp/sp_fanout_34.json"),
}

COMMON = {
    "piecetab.h": ("fragmented_100k", {"seek": "pt_seek", "locate": "pt_locate", "advance": "pt_advance", "splice": "pt_splice"}),
    "linecache.h": ("lines_100k", {"seek": "lc_seek", "locate": "lc_locate", "advance": "lc_advance", "splice": "lc_splice"}),
    "spantree.h": ("fragmented_100k", {"seek": "sp_seek", "locate": "sp_locate", "advance": "sp_advance", "splice": "sp_splice"}),
}

NEXT = {
    "piecetab.h": ("fragmented_100k", "pt_next"),
    "spantree.h": ("fragmented_100k", "sp_next"),
}

UNIQUE = {
    "piecetab.h": ("fragmented_100k", "pt_edit", "edit"),
    "linecache.h": ("lines_100k", "lc_scan", "scan (per line)"),
    "spantree.h": ("fragmented_100k", "sp_fill", "fill"),
}


def metric(c):
    if c.get("amortized_ns", 0) > 0:
        return c["amortized_ns"]
    return c["ns_per_op"]


def load_cases(path):
    with open(path) as f:
        return json.load(f)["cases"]


def find(path, name, corpus):
    for c in load_cases(path):
        if c["name"] == name and c["corpus"] == corpus:
            return metric(c)
    return None


def fmt(v):
    if v is None:
        return "-"
    if v >= 100:
        return "%.1f" % v
    if v >= 10:
        return "%.2f" % v
    return "%.3f" % v


def main():
    libs = ("piecetab.h", "linecache.h", "spantree.h")
    data = {}
    for lib in libs:
        path = FILES[lib]
        corpus, names = COMMON[lib]
        uniq_corpus, uniq_name, uniq_label = UNIQUE[lib]
        nxt = None
        if lib in NEXT:
            ncorpus, nname = NEXT[lib]
            nxt = find(path, nname, ncorpus)
        data[lib] = {
            "seek": find(path, names["seek"], corpus),
            "locate": find(path, names["locate"], corpus),
            "advance": find(path, names["advance"], corpus),
            "splice": find(path, names["splice"], corpus),
            "next": nxt,
            "unique_label": uniq_label,
            "unique": find(path, uniq_name, uniq_corpus),
        }

    rows = [
        ("seek", "seek", None),
        ("locate", "locate", None),
        ("advance", "advance", None),
        ("splice", "splice", None),
        ("next (per item)", "next", None),
        ("edit", "unique", "piecetab.h"),
        ("scan (per line)", "unique", "linecache.h"),
        ("fill", "unique", "spantree.h"),
    ]

    print("| Operation | piecetab.h | linecache.h | spantree.h |")
    print("| --- | ---: | ---: | ---: |")
    for label, key, only in rows:
        cells = []
        for lib in libs:
            if only is not None and lib != only:
                cells.append("-")
            elif key == "unique":
                cells.append(fmt(data[lib]["unique"]))
            else:
                cells.append(fmt(data[lib][key]))
        print("| %s | %s | %s | %s |" % (label, cells[0], cells[1], cells[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
