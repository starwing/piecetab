#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Wconversion -Wno-sign-conversion -Werror -pedantic -std=c89 -Wno-variadic-macros}"
# FANOUT=64 is excluded: ptM_mask(64) is UB even with unsigned long long.
FANOUTS="${FANOUTS:-$(seq 4 63)}"
ROUNDS="${BENCH_ROUNDS:-7}"
ITERS="${BENCH_ITERS:-}"

# Safe PT_MAX_LEVEL for 64-bit size_t, worst-case half-full B+ tree.
# Generated from MAX_LEVELS = ceil(log_{FANOUT/2}(SIZE_MAX)).
# Uses a case statement instead of an associative array for macOS bash 3.2.
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

mkdir -p bench/build bench/results
GIT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"

for f in $FANOUTS; do
    echo "== FANOUT $f =="
    safe=$(safe_max_levels "$f")
    LONGLONG_FLAG=""
    if [ "$f" -ge 32 ]; then
        # PT_FANOUT >= 32 uses unsigned long long; keep the bench build
        # usable even with -pedantic -std=c89 by suppressing that extension warning.
        LONGLONG_FLAG="-Wno-long-long"
    fi
    $CC $CFLAGS $LONGLONG_FLAG -O2 -DNDEBUG -DPT_FANOUT="$f" \
        -DBENCH_GIT="\"$GIT\"" \
        -DPT_MAX_LEVEL="$safe" \
        -I. -Ibench -o "bench/build/pt_fanout_$f" bench/bench_pt.c
    ./bench/build/pt_fanout_$f --seed 1 --rounds "$ROUNDS" \
        ${ITERS:+--iters "$ITERS"} \
        ${BENCH_ARGS:-} --json "bench/results/pt_fanout_$f.json"
done

echo "sweep complete: $(echo $FANOUTS | wc -w) fanouts -> bench/results/"
