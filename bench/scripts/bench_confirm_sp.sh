#!/usr/bin/env bash
set -euo pipefail

# Confirmation sweep for SP_FANOUT: multiple seeds over a focused FANOUT set.
# Outputs bench/results/sp/confirm/sp_fanout_<f>_seed<s>.json

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Wconversion -Wno-sign-conversion -Werror -pedantic -std=c89 -Wno-variadic-macros}"
FANOUTS="${FANOUTS:-28 30 31 32 34 36 38 40}"
SEEDS="${BENCH_SEEDS:-1 2 3}"
ROUNDS="${BENCH_ROUNDS:-5}"
ITERS="${BENCH_ITERS:-}"
MIN_SECONDS="${BENCH_MIN_SECONDS:-0.5}"

safe_max_levels() {
    case "$1" in
        4|5) echo 64 ;;
        6|7) echo 41 ;;
        8|9) echo 32 ;;
        10|11) echo 28 ;;
        12|13) echo 25 ;;
        14|15) echo 23 ;;
        16|17) echo 22 ;;
        18|19) echo 21 ;;
        20|21) echo 20 ;;
        22|23) echo 19 ;;
        24|25|26|27) echo 18 ;;
        28|29|30|31) echo 17 ;;
        32|33|34|35|36|37|38|39) echo 16 ;;
        40|41|42|43|44|45|46|47) echo 15 ;;
        48|49|50|51|52|53|54|55|56|57|58|59|60|61) echo 14 ;;
        62|63|64) echo 13 ;;
        *) echo 13 ;;
    esac
}

mkdir -p bench/build/sp bench/results/sp/confirm
GIT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"

for f in $FANOUTS; do
    echo "== SP_FANOUT $f =="
    safe=$(safe_max_levels "$f")
    bin="bench/build/sp/sp_confirm_$f"
    $CC $CFLAGS -O2 -DNDEBUG -DSP_FANOUT="$f" \
        -DBENCH_GIT="\"$GIT\"" \
        -DSP_MAX_LEVEL="$safe" \
        -I. -Ibench -o "$bin" bench/bench_sp.c
    for s in $SEEDS; do
        echo "  seed $s"
        ./"$bin" --seed "$s" --rounds "$ROUNDS" \
            --min-seconds "$MIN_SECONDS" \
            ${ITERS:+--iters "$ITERS"} \
            ${BENCH_ARGS:-} --json "bench/results/sp/confirm/sp_fanout_${f}_seed${s}.json"
    done
done

echo "confirmation complete: $(echo $FANOUTS | wc -w) fanouts x $(echo $SEEDS | wc -w) seeds -> bench/results/sp/confirm/"
