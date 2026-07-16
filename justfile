# linecache test runner

CC := "gcc"
CFLAGS := "-Wall -Wextra -Wconversion -Wno-sign-conversion -Werror -pedantic -std=c89 -Wno-variadic-macros"
INCS := "-I. -Itests"

cov: lc-cov cov-uncovered lc-lines

dbg-run t *tests='':
    {{ CC }} {{ CFLAGS }} {{ INCS }} -g -O0 -fsanitize=address,undefined -o tests/{{ t }} tests/{{ t }}.c && ./tests/{{ t }} {{ tests }}

cov-run t *tests='':
    {{ CC }} {{ CFLAGS }} {{ INCS }} --coverage -Wno-unused-function -O0 -o tests/{{ t }} tests/{{ t }}.c && ./tests/{{ t }} {{ tests }}

clean-gcda:
    rm -f tests/*.gcda tests/*.gcno ./*.gcda ./*.gcno *.info

clean: clean-gcda
    rm -f tests/lc_test4 tests/lc_test8 tests/pt_test4
    rm -fr tests/*.dSYM

cov-show src:
    lcov --capture --directory . --rc branch_coverage=1 --output-file coverage.info --no-external --ignore-errors unsupported
    lcov --extract coverage.info '*/{{ src }}' --rc branch_coverage=1  --output-file lcov.info
    @echo ""
    @echo "=== {{ src }} coverage ==="
    lcov --list --rc branch_coverage=1  lcov.info

cov-lines src:
    @awk '/^DA:/ && /,0$/ {gsub(/DA:|,0/,""); print $0}' lcov.info \
    | sort -n | while read ln; do echo "L$ln: $(sed -n ${ln}p {{ src }})"; done

cov-unbranched src:
    @awk -F'[:,]' '/^BRDA:/ && $5==0 {print $2}' lcov.info \
    | sort -n -u \
    | while read ln; do \
        src=$(sed -n "${ln}p" {{ src }} 2>/dev/null); \
        case "$src" in *assert*) ;; *) printf "L%-5d %s\n" "$ln" "$src";; esac; \
    done

cov-uncovered:
    @awk '/^DA:/ && /,0$/ {gsub(/DA:|,0/,""); print $0}' lcov.info \
    | sort -n | awk \
    'NR==1{s=p=$1;c=1} $1==p+1{p=$1;c++} \
    $1>p+1{printf "%4d-%-4d (%d lines)\n",s,p,c;s=$1;p=$1;c=1} \
    END{printf "%4d-%-4d (%d lines)\n",s,p,c}'

# linecache tests
lc *tests='': (dbg-run "lc_test4" tests)
# large fanout tests (LC_LEAF_FANOUT=8, LC_FANOUT=8)
lc8 *tests='': (dbg-run "lc_test8" tests)
lc-cov: clean-gcda (cov-run "lc_test4") (cov-run "lc_test8") (cov-show "linecache.h")
lc-lines: (cov-lines "linecache.h")
lc-unbranched: (cov-unbranched "linecache.h")

# piecetab tests
pt *tests='': (dbg-run "pt_test4" tests)
pt-cov: clean-gcda (cov-run "pt_test4") (cov-show "piecetab.h")
pt-lines: (cov-lines "piecetab.h")
pt-unbranched: (cov-unbranched "piecetab.h")
