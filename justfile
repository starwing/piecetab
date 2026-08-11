import 'build.just'

# C tests: one runner per lib, parametrized. Lua side lives in lua/justfile.

INCS := "-I. -Itests"

test t *args='': (dbg-run t INCS args)

lc *args='': (test "tests/linecache_test_fanout4" args)
lc8 *args='': (test "tests/linecache_test_fanout8" args)
pt *args='': (test "tests/piecetab_test_fanout4" args)
ut *args='': (test "tests/undotree_test" args)
cg *args='': (test "tests/cellgrid_test" args)
tf *args='': (test "tests/termfeed_test" args)

# coverage

cov: clean-gcda (cov-run "tests/linecache_test_fanout4" INCS) (cov-run "tests/linecache_test_fanout8" INCS) (cov-run "tests/piecetab_test_fanout4" INCS) (cov-run "tests/undotree_test" INCS) (cov-run "tests/cellgrid_test" INCS) (cov-run "tests/termfeed_test" INCS) (cov-show "piecetab.h linecache.h undotree.h cellgrid.h termfeed.h")

lc-cov: clean-gcda (cov-run "tests/linecache_test_fanout4" INCS) (cov-run "tests/linecache_test_fanout8" INCS) (cov-show "linecache.h")
pt-cov: clean-gcda (cov-run "tests/piecetab_test_fanout4" INCS) (cov-show "piecetab.h")
ut-cov: clean-gcda (cov-run "tests/undotree_test" INCS) (cov-show "undotree.h")
cg-cov: clean-gcda (cov-run "tests/cellgrid_test" INCS) (cov-show "cellgrid.h")
tf-cov: clean-gcda (cov-run "tests/termfeed_test" INCS) (cov-show "termfeed.h")

lc-lines: (cov-lines "linecache.h")
pt-lines: (cov-lines "piecetab.h")
ut-lines: (cov-lines "undotree.h")
cg-lines: (cov-lines "cellgrid.h")
tf-lines: (cov-lines "termfeed.h")

lc-unbranched: (cov-unbranched "linecache.h")
pt-unbranched: (cov-unbranched "piecetab.h")
ut-unbranched: (cov-unbranched "undotree.h")
cg-unbranched: (cov-unbranched "cellgrid.h")
tf-unbranched: (cov-unbranched "termfeed.h")

# lua bindings & tests live in lua/justfile — run with: just lua/<recipe>

clean: clean-gcda
    rm -f tests/linecache_test_fanout4 tests/linecache_test_fanout8 tests/piecetab_test_fanout4 tests/undotree_test tests/cellgrid_test tests/termfeed_test
    rm -fr tests/*.dSYM
    rm -fr build

# LuaLS type check on lua sources (target: zero warnings in lsp.lua/editor.lua/tests)

luals:
    rm -rf /tmp/luals_check
    mkdir -p /tmp/luals_check/log
    lua-language-server --check . --checklevel=Warning --configpath=.luarc.json --logpath=/tmp/luals_check/log
