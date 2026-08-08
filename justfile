import 'build.just'

# piecetab core tests (cellgrid & termfeed live in mods/, delegated below)

INCS := "-I. -Itests"

lc *tests='': (dbg-run "tests/lc_test4" "-I. -Itests" tests)
# large fanout tests (LC_LEAF_FANOUT=8, LC_FANOUT=8)
lc8 *tests='': (dbg-run "tests/lc_test8" "-I. -Itests" tests)
pt *tests='': (dbg-run "tests/pt_test4" "-I. -Itests" tests)
ut *tests='': (dbg-run "tests/ut_test" "-I. -Itests" tests)

# mods: delegate to per-module justfiles

cg *tests='': (mod-test "cellgrid" tests)
tf *tests='': (mod-test "termfeed" tests)

mod-test mod *tests='':
    @just -f mods/{{ mod }}/justfile test {{ tests }}

lua-cg: (mod-lua "cellgrid")
lua-tf: (mod-lua "termfeed")
lua-cg-cov: (mod-lua-cov "cellgrid")
lua-tf-cov: (mod-lua-cov "termfeed")
lua-cg-lines: (mod-lua-lines "cellgrid")
lua-tf-lines: (mod-lua-lines "termfeed")

mod-lua mod:
    @just -f mods/{{ mod }}/justfile lua

mod-lua-cov mod:
    @just -f mods/{{ mod }}/justfile lua-cov

mod-lua-lines mod:
    @just -f mods/{{ mod }}/justfile lua-lines

# coverage

cov: clean-gcda (cov-run "tests/lc_test4" "-I. -Itests") (cov-run "tests/lc_test8" "-I. -Itests") (cov-run "tests/pt_test4" "-I. -Itests") (cov-run "tests/ut_test" "-I. -Itests") (cov-run "mods/cellgrid/cellgrid_test" "-Imods/cellgrid -I.") (cov-run "mods/termfeed/termfeed_test" "-Imods/termfeed -I.") (cov-show "piecetab.h linecache.h undotree.h mods/cellgrid/cellgrid.h mods/termfeed/termfeed.h")

lc-cov: clean-gcda (cov-run "tests/lc_test4" "-I. -Itests") (cov-run "tests/lc_test8" "-I. -Itests") (cov-show "linecache.h")
pt-cov: clean-gcda (cov-run "tests/pt_test4" "-I. -Itests") (cov-show "piecetab.h")
ut-cov: clean-gcda (cov-run "tests/ut_test" "-I. -Itests") (cov-show "undotree.h")
cg-cov: clean-gcda (cov-run "mods/cellgrid/cellgrid_test" "-Imods/cellgrid -I.") (cov-show "mods/cellgrid/cellgrid.h")
tf-cov: clean-gcda (cov-run "mods/termfeed/termfeed_test" "-Imods/termfeed -I.") (cov-show "mods/termfeed/termfeed.h")

lc-lines: (cov-lines "linecache.h")
pt-lines: (cov-lines "piecetab.h")
ut-lines: (cov-lines "undotree.h")
cg-lines: (cov-lines "mods/cellgrid/cellgrid.h")
tf-lines: (cov-lines "mods/termfeed/termfeed.h")

lc-unbranched: (cov-unbranched "linecache.h")
pt-unbranched: (cov-unbranched "piecetab.h")
ut-unbranched: (cov-unbranched "undotree.h")
cg-unbranched: (cov-unbranched "mods/cellgrid/cellgrid.h")
tf-unbranched: (cov-unbranched "mods/termfeed/termfeed.h")

clean: clean-gcda
    rm -f tests/lc_test4 tests/lc_test8 tests/pt_test4 tests/ut_test
    rm -f mods/cellgrid/cellgrid_test mods/termfeed/termfeed_test
    rm -fr tests/*.dSYM

# piecetab lua binding (endpoints: PUC 5.5 + LuaJIT 2.1/5.1 cover the compat range)

lua-pt-build:
    mkdir -p build/lua55 build/luajit
    {{ CC }} {{ LUAFLAGS }} {{ INCS }} {{ LUA55_INC }} -DNDEBUG -O2 -bundle -undefined dynamic_lookup -o build/lua55/piecetab.so piecetab.c
    {{ CC }} {{ LUAFLAGS }} {{ INCS }} {{ LUAJIT_INC }} -DNDEBUG -O2 -bundle -undefined dynamic_lookup -o build/luajit/piecetab.so piecetab.c

lua-pt *t: lua-pt-build
    lua tests/lua/pt_test.lua {{ t }}
    luajit tests/lua/pt_test.lua {{ t }}

lua-pt-cov: clean-gcda
    mkdir -p build/lua55 build/luajit
    {{ CC }} {{ LUAFLAGS }} {{ INCS }} {{ LUA55_INC }} --coverage -g -O0 -bundle -undefined dynamic_lookup -o build/lua55/piecetab.so piecetab.c
    {{ CC }} {{ LUAFLAGS }} {{ INCS }} {{ LUAJIT_INC }} --coverage -g -O0 -bundle -undefined dynamic_lookup -o build/luajit/piecetab.so piecetab.c
    lua tests/lua/pt_test.lua && luajit tests/lua/pt_test.lua
    lcov --capture --directory build --rc branch_coverage=1 --output-file lua_coverage.info --ignore-errors mismatch
    lcov --extract lua_coverage.info '*/piecetab.c' --rc branch_coverage=1 --output-file lcov.info
    @echo ""
    @echo "=== piecetab.c coverage ==="
    lcov --list --rc branch_coverage=1 lcov.info

lua-pt-lines:
    @awk '/^DA:/ && /,0$/ {gsub(/DA:|,0/,""); print $0}' lcov.info \
    | sort -n | while read ln; do echo "L$ln: $(sed -n ${ln}p piecetab.c)"; done
