# linecache test runner

CC := "gcc"
CFLAGS := "-Wall -Wextra -Werror -pedantic -std=c89 -Wno-variadic-macros"
INCS := "-I. -Itests"

dbg *tests='':
    {{ CC }} {{ CFLAGS }} {{ INCS }} -g -O0 -fsanitize=address,undefined -o tests/lc_test4 tests/lc_test4.c && ./tests/lc_test4 {{ tests }}

# large fanout tests (LC_LEAF_FANOUT=8, LC_FANOUT=8)
dbg_lc8 *tests='':
    {{ CC }} {{ CFLAGS }} {{ INCS }} -g -O0 -fsanitize=address,undefined -o tests/lc_test8 tests/lc_test8.c && ./tests/lc_test8 {{ tests }}

cov_run t:
    {{ CC }} {{ CFLAGS }} {{ INCS }} --coverage -O0 -o tests/{{ t }} tests/{{ t }}.c && ./tests/{{ t }}

lc-cov: clean-gcda (cov_run "lc_test4") (cov_run "lc_test8")
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
    rm -f tests/*.gcda tests/*.gcno ./*.gcda ./*.gcno *.info

clean: clean-gcda
    rm -f tests/lc_test4 tests/lc_test8
