#!/usr/bin/env bash
set -euo pipefail
set +H

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Wconversion -Wno-sign-conversion -Werror -pedantic -std=c89 -Wno-variadic-macros}"
MODE="${1:-fanout}"
FIXED="${2:-62}"
VALUES="${LC_VALUES:-$(seq 4 63)}"
ROUNDS="${BENCH_ROUNDS:-3}"
ITERS="${BENCH_ITERS:-}"
MIN_SECONDS="${BENCH_MIN_SECONDS:-0.2}"

safe_lc_max_levels() {
    python3 -c 'import math,sys
f=int(sys.argv[1]); lf=int(sys.argv[2]); b=f//2; leaf=lf//2; m=(1<<64)-1
print(int(math.ceil(math.log(float(m)/leaf, b)))+1)' "$1" "$2"
}

if [ "$MODE" = "fanout" ]; then
    OUTDIR="bench/results/lc/fanout"
    mkdir -p "bench/build/lc" "$OUTDIR"
    for f in $VALUES; do
        echo "== LC_FANOUT=$f LC_LEAF_FANOUT=$FIXED =="
        safe=$(safe_lc_max_levels "$f" "$FIXED")
        bin="bench/build/lc/lc_fanout_${f}"
        $CC $CFLAGS -O2 -DNDEBUG -DLC_FANOUT="$f" -DLC_LEAF_FANOUT="$FIXED" \
            -DLC_MAX_LEVEL="$safe" \
            -DBENCH_GIT="\"$(git rev-parse --short HEAD 2>/dev/null || echo unknown)\"" \
            -I. -Ibench -o "$bin" bench/bench_lc.c
        ./"$bin" --seed 1 --rounds "$ROUNDS" \
            --min-seconds "$MIN_SECONDS" \
            ${ITERS:+--iters "$ITERS"} \
            ${BENCH_ARGS:-} --json "$OUTDIR/lc_f${f}_lf${FIXED}.json"
    done
elif [ "$MODE" = "leaf" ]; then
    OUTDIR="bench/results/lc/leaf"
    mkdir -p "bench/build/lc" "$OUTDIR"
    for lf in $VALUES; do
        echo "== LC_FANOUT=$FIXED LC_LEAF_FANOUT=$lf =="
        safe=$(safe_lc_max_levels "$FIXED" "$lf")
        bin="bench/build/lc/lc_leaf_${lf}"
        $CC $CFLAGS -O2 -DNDEBUG -DLC_FANOUT="$FIXED" -DLC_LEAF_FANOUT="$lf" \
            -DLC_MAX_LEVEL="$safe" \
            -DBENCH_GIT="\"$(git rev-parse --short HEAD 2>/dev/null || echo unknown)\"" \
            -I. -Ibench -o "$bin" bench/bench_lc.c
        ./"$bin" --seed 1 --rounds "$ROUNDS" \
            --min-seconds "$MIN_SECONDS" \
            ${ITERS:+--iters "$ITERS"} \
            ${BENCH_ARGS:-} --json "$OUTDIR/lc_f${FIXED}_lf${lf}.json"
    done
else
    echo "usage: $0 [fanout|leaf] [fixed_value]" >&2
    exit 2
fi

echo "lc sweep complete: mode=$MODE fixed=$FIXED -> $OUTDIR/"
