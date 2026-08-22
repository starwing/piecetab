#!/usr/bin/env bash
set -euo pipefail
set +H

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Wconversion -Wno-sign-conversion -Werror -pedantic -std=c89 -Wno-variadic-macros}"
COMBOS="${LC_COMBOS:-16:32 16:34 16:40 17:32 17:34 17:40 24:32 24:34 31:34}"
SEEDS="${BENCH_SEEDS:-1 2 3}"
ROUNDS="${BENCH_ROUNDS:-5}"
ITERS="${BENCH_ITERS:-}"
MIN_SECONDS="${BENCH_MIN_SECONDS:-0.3}"

safe_lc_max_levels() {
    python3 -c 'import math,sys
f=int(sys.argv[1]); lf=int(sys.argv[2]); b=f//2; leaf=lf//2; m=(1<<64)-1
print(int(math.ceil(math.log(float(m)/leaf, b)))+1)' "$1" "$2"
}

mkdir -p bench/build/lc bench/results/lc/confirm
GIT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"

for combo in $COMBOS; do
    f="${combo%%:*}"
    lf="${combo##*:}"
    echo "== LC_FANOUT=$f LC_LEAF_FANOUT=$lf =="
    safe=$(safe_lc_max_levels "$f" "$lf")
    bin="bench/build/lc/lc_confirm_${f}_${lf}"
    $CC $CFLAGS -O2 -DNDEBUG -DLC_FANOUT="$f" -DLC_LEAF_FANOUT="$lf" \
        -DLC_MAX_LEVEL="$safe" \
        -DBENCH_GIT="\"$GIT\"" \
        -I. -Ibench -o "$bin" bench/bench_lc.c
    for s in $SEEDS; do
        echo "  seed $s"
        ./"$bin" --seed "$s" --rounds "$ROUNDS" \
            --min-seconds "$MIN_SECONDS" \
            ${ITERS:+--iters "$ITERS"} \
            ${BENCH_ARGS:-} --json "bench/results/lc/confirm/lc_f${f}_lf${lf}_seed${s}.json"
    done
done

echo "lc confirm complete: $COMBOS -> bench/results/lc/confirm/"
