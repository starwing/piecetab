#!/usr/bin/env bash
set -euo pipefail
set +H

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Wconversion -Wno-sign-conversion -Werror -pedantic -std=c89 -Wno-variadic-macros}"
FANOUTS="${LC_FANOUTS:-16 24 31 34 40 62}"
LEAFS="${LC_LEAFS:-16 24 31 34 40 62}"
ROUNDS="${BENCH_ROUNDS:-2}"
ITERS="${BENCH_ITERS:-}"
MIN_SECONDS="${BENCH_MIN_SECONDS:-0.1}"
OUTDIR="${LC_OUTDIR:-bench/results/lc/probe}"

safe_lc_max_levels() {
    python3 -c 'import math,sys
f=int(sys.argv[1]); lf=int(sys.argv[2]); b=f//2; leaf=lf//2; m=(1<<64)-1
print(int(math.ceil(math.log(float(m)/leaf, b)))+1)' "$1" "$2"
}

mkdir -p bench/build/lc "$OUTDIR"
GIT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"

for f in $FANOUTS; do
    for lf in $LEAFS; do
        echo "== LC_FANOUT=$f LC_LEAF_FANOUT=$lf =="
        safe=$(safe_lc_max_levels "$f" "$lf")
        bin="bench/build/lc/lc_probe_${f}_${lf}"
        $CC $CFLAGS -O2 -DNDEBUG -DLC_FANOUT="$f" -DLC_LEAF_FANOUT="$lf" \
            -DLC_MAX_LEVEL="$safe" \
            -DBENCH_GIT="\"$GIT\"" \
            -I. -Ibench -o "$bin" bench/bench_lc.c
        ./"$bin" --seed 1 --rounds "$ROUNDS" \
            --min-seconds "$MIN_SECONDS" \
            ${ITERS:+--iters "$ITERS"} \
            ${BENCH_ARGS:-} --json "$OUTDIR/lc_f${f}_lf${lf}.json"
    done
done

echo "lc probe complete: $(echo $FANOUTS | wc -w) x $(echo $LEAFS | wc -w) -> $OUTDIR/"
