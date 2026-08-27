# Shared build recipes — imported by root justfile and submodule justfiles.
# All compiler/linker variables and every compile command live here.
# Recipes are directory-independent: ROOT is build.just's directory
# (source_directory(), not justfile_dir(), which depends on invocation style);
# submodule justfiles pass paths relative to their own CWD.

ROOT := source_directory()

CC := "gcc"
CFLAGS := "-Wall -Wextra -Wconversion -Wno-sign-conversion -Werror -pedantic -std=c89 -Wno-variadic-macros"
LUAFLAGS := "-Wall -Wextra -Wconversion -Wno-sign-conversion -Werror -std=gnu99 -Wno-variadic-macros -Wno-array-bounds"
LDF := `if [ "$(uname)" = "Darwin" ]; then echo "-bundle -undefined dynamic_lookup"; else echo "-shared -fPIC"; fi`
LUA_INC := `
    if [ "$(uname)" = "Darwin" ]; then
        for d in /opt/homebrew/include/lua5.5 /opt/homebrew/include/lua5.4 \
            "$HOME/.lua/include" "$HOME/.lua/include/lua5.5" \
            "$HOME/.lua/include/lua5.4" ../.lua/include .lua/include \
            /usr/local/include /usr/local/include/lua5.5 \
            /usr/local/include/lua5.4 /usr/include/lua5.4; do
            [ -d "$d" ] && { echo "-I$d"; exit 0; }
        done
    else
        for d in ../.lua/include .lua/include "$HOME/.lua/include" \
            "$HOME/.lua/include/lua5.5" "$HOME/.lua/include/lua5.4" \
            /usr/local/include /usr/local/include/lua5.5 \
            /usr/local/include/lua5.4 /usr/include/lua5.4; do
            [ -d "$d" ] && { echo "-I$d"; exit 0; }
        done
    fi
    exit 0
`
LUAJIT_INC := `
    for d in ../.lua/include/luajit-2.1 .lua/include/luajit-2.1 \
        /opt/homebrew/include/luajit-2.1 /usr/include/luajit-2.1 \
        /usr/local/include/luajit-2.1; do
        [ -d "$d" ] && { echo "-I$d"; exit 0; }
    done
    exit 0
`
TS_INC := "-I/opt/homebrew/include"
TS_LIB := "-L/opt/homebrew/lib -ltree-sitter"

[private]
py script *args='':
    @if [ -x {{ ROOT }}/.venv/bin/python ]; then \
        {{ ROOT }}/.venv/bin/python {{ script }} {{ args }}; \
    else \
        python3 {{ script }} {{ args }}; \
    fi

[private]
rm-gcda dir:
    -rm -f {{ dir }}/*.gcda {{ dir }}/*.gcno {{ dir }}/*.gcov {{ dir }}/*.info

[private]
c-build src out incs extra='':
    @mkdir -p $(dirname {{ out }})
    {{ CC }} {{ CFLAGS }} {{ incs }} -O2 -DNDEBUG {{ extra }} -o {{ out }} {{ src }}

[private]
c-dbg-run t incs *args='':
    lua {{ ROOT }}/misc/gen_entries.lua {{ t }}.c {{ t }}.gen.inc
    {{ CC }} {{ CFLAGS }} {{ incs }} -g -O0 -fsanitize=address,undefined -fno-sanitize-recover=undefined -o {{ t }} {{ t }}.c && ./{{ t }} {{ args }}

[private]
c-cov-run t incs *tests='':
    lua {{ ROOT }}/misc/gen_entries.lua {{ t }}.c {{ t }}.gen.inc
    {{ CC }} {{ CFLAGS }} {{ incs }} -DNDEBUG --coverage -Wno-unused-function -O0 -o {{ t }} {{ t }}.c && ./{{ t }} {{ tests }}

[private]
fuzz-run lib incs mode arg='':
    {{ CC }} {{ CFLAGS }} {{ incs }} -g -O2 -o {{ lib }}_fuzz {{ lib }}_fuzz.c && ./{{ lib }}_fuzz {{ mode }} {{ arg }}

[private]
dfz-run lib incs mode arg='':
    {{ CC }} {{ CFLAGS }} {{ incs }} -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -o {{ lib }}_fuzz_dbg {{ lib }}_fuzz.c && ./{{ lib }}_fuzz_dbg {{ mode }} {{ arg }}

[private]
lua-build src incs lua_inc out extra='' libs='':
    @mkdir -p $(dirname {{ out }})
    {{ CC }} {{ LUAFLAGS }} {{ incs }} {{ lua_inc }} {{ extra }} {{ LDF }} -o {{ out }} {{ src }} {{ libs }}

[private]
lua-cov-run mod t incs libs='':
    rm -f *.gcda luajit/*.gcda
    @mkdir -p luajit
    {{ CC }} {{ LUAFLAGS }} {{ incs }} {{ LUA_INC }} --coverage -g -O0 {{ LDF }} -o {{ mod }}.so {{ mod }}.c {{ libs }}
    {{ CC }} {{ LUAFLAGS }} {{ incs }} {{ LUAJIT_INC }} --coverage -g -O0 {{ LDF }} -o luajit/{{ mod }}.so {{ mod }}.c {{ libs }}
    lua tests/{{ t }}.lua && luajit tests/{{ t }}.lua
    @just cov-show "." "{{ mod }}.c"

[private]
cov-gen dir src lcov=(dir / 'lcov.info'):
    lcov --capture --directory {{ dir }} --rc branch_coverage=1 --output-file coverage.info \
        --no-external --ignore-errors unsupported,inconsistent
    lcov --extract coverage.info {{ src }} --rc branch_coverage=1 --ignore-errors inconsistent --output-file {{ lcov }}

[private]
cov-show dir src lcov=(dir / 'lcov.info'): (cov-gen dir src lcov)
    @echo "=== {{ src }} coverage ==="
    lcov --ignore-errors inconsistent --list --rc branch_coverage=1 {{ lcov }}
    @echo ""

[private]
cov-lines src lcov='lcov.info':
    @awk '/^DA:/ && /,0$/ {gsub(/DA:|,0/,""); print $0}' {{ lcov }} \
    | sort -n | while read ln; do \
        src=$(sed -n "${ln}p" {{ src }} 2>/dev/null); \
        case "$src" in *assert*) ;; *) echo "L$ln: $src";; esac; \
    done

[private]
cov-unbranched src lcov='lcov.info':
    @awk -F'[:,]' '/^BRDA:/ && $5==0 {print $2}' {{ lcov }} \
    | sort -n -u \
    | while read ln; do \
        src=$(sed -n "${ln}p" {{ src }} 2>/dev/null); \
        case "$src" in *assert*) ;; *) printf "L%-5d %s\n" "$ln" "$src";; esac; \
    done

[private]
cov-uncovered lcov='lcov.info':
    @awk '/^DA:/ && /,0$/ {gsub(/DA:|,0/,""); print $0}' {{ lcov }} \
    | sort -n | awk \
    'NR==1{s=p=$1;c=1} $1==p+1{p=$1;c++} \
    $1>p+1{printf "%4d-%-4d (%d lines)\n",s,p,c;s=$1;p=$1;c=1} \
    END{printf "%4d-%-4d (%d lines)\n",s,p,c}'
