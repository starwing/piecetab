# marktree test runner

CC := "gcc"
CFLAGS := "-Wall -Wextra -Werror -pedantic -std=c89 -Wno-variadic-macros"

dbg *tests='':
    {{ CC }} {{ CFLAGS }} -Wno-variadic-macros -g -O0 -fsanitize=address,undefined -o linecache_test linecache_test.c && ./linecache_test {{ tests }}

# large fanout tests (LC_LEAF_FANOUT=8, LC_FANOUT=8)
dbg_lc8 *tests='':
    {{ CC }} {{ CFLAGS }} -g -O0 -fsanitize=address,undefined -o linecache_test_large linecache_test_large.c && ./linecache_test_large {{ tests }}

cov_run src:
    {{ CC }} {{ CFLAGS }} --coverage -O0 -o {{ src }} {{ src }}.c && ./{{ src }}

lc-cov: clean-gcda (cov_run "linecache_test") (cov_run "linecache_test_large")
    lcov --capture --directory . --output-file coverage.info --no-external --ignore-errors unsupported
    lcov --extract coverage.info '*/linecache.h' --output-file lcov.info
    @echo ""
    @echo "=== linecache.h coverage ==="
    lcov --list lcov.info

lc-uncovered:
    awk '/^DA:/ && /,0$/ {gsub(/DA:|,0/,""); print $0}' lcov.info \
    | sort -n | awk \
    'NR==1{s=p=$1;c=1} $1==p+1{p=$1;c++} \
    $1>p+1{printf "%4d-%-4d (%d lines)\n",s,p,c;s=$1;p=$1;c=1} \
    END{printf "%4d-%-4d (%d lines)\n",s,p,c}'

lc-lines:
    @awk '/^DA:/ && /,0$/ {gsub(/DA:|,0/,""); print $0}' lcov.info \
    | sort -n | while read ln; do echo "L$ln: $(sed -n ${ln}p linecache.h)"; done

cov: lc-cov lc-uncovered lc-lines

clean-gcda:
    rm -f ./*.gcda ./*.gcno ./*.info

clean: clean-gcda
    rm -f linecache_test linecache_test_large
