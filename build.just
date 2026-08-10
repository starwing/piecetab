# shared build recipes — imported by root justfile and mods/*/justfile
# mods recipes run with CWD = mods/<name> (just -f sets it)

CC := "gcc"
CFLAGS := "-Wall -Wextra -Wconversion -Wno-sign-conversion -Werror -pedantic -std=c89 -Wno-variadic-macros"
LUAFLAGS := "-Wall -Wextra -Wconversion -Wno-sign-conversion -Werror -std=c89 -Wno-variadic-macros"
LUA55_INC := "-I/opt/homebrew/include/lua5.5"
LUAJIT_INC := "-I/opt/homebrew/include/luajit-2.1"

# t = test source without .c suffix (may include dir), inc = include flags
dbg-run t inc *tests='':
    lua misc/gen_entries.lua {{ t }}.c {{ t }}.gen.inc
    {{ CC }} {{ CFLAGS }} {{ inc }} -g -O0 -fsanitize=address,undefined -o {{ t }} {{ t }}.c && ./{{ t }} {{ tests }}

cov-run t inc *tests='':
    lua misc/gen_entries.lua {{ t }}.c {{ t }}.gen.inc
    {{ CC }} {{ CFLAGS }} {{ inc }} -DNDEBUG --coverage -Wno-unused-function -O0 -o {{ t }} {{ t }}.c && ./{{ t }} {{ tests }}

clean-gcda:
    rm -f tests/*.gcda tests/*.gcno ./*.gcda ./*.gcno *.info build/*/*.gcda build/*/*.gcno lua/*.gcda lua/*.gcno lua/luajit/*.gcda lua/luajit/*.gcno

cov-show src:
    lcov --capture --directory . --rc branch_coverage=1 --output-file coverage.info --no-external --ignore-errors unsupported,inconsistent --filter range
    lcov --extract coverage.info {{ src }} --rc branch_coverage=1 --ignore-errors inconsistent --output-file lcov.info
    @echo ""
    @echo "=== {{ src }} coverage ==="
    lcov --ignore-errors inconsistent --list --rc branch_coverage=1 lcov.info

cov-lines src:
    @awk '/^DA:/ && /,0$/ {gsub(/DA:|,0/,""); print $0}' lcov.info \
    | sort -n | while read ln; do \
        src=$(sed -n "${ln}p" {{ src }} 2>/dev/null); \
        case "$src" in *assert*) ;; *) echo "L$ln: $src";; esac; \
    done

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
