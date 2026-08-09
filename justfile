import 'build.just'

# C tests: one runner per lib, parametrized. Lua side lives in lua/justfile.

INCS := "-I. -Itests"

test t *args='': (dbg-run t INCS args)

lc *args='': (test "tests/lc_test4" args)
lc8 *args='': (test "tests/lc_test8" args)
pt *args='': (test "tests/pt_test4" args)
ut *args='': (test "tests/ut_test" args)
cg *args='': (test "tests/cellgrid_test" args)
tf *args='': (test "tests/termfeed_test" args)

# coverage

cov: clean-gcda (cov-run "tests/lc_test4" INCS) (cov-run "tests/lc_test8" INCS) (cov-run "tests/pt_test4" INCS) (cov-run "tests/ut_test" INCS) (cov-run "tests/cellgrid_test" INCS) (cov-run "tests/termfeed_test" INCS) (cov-show "piecetab.h linecache.h undotree.h cellgrid.h termfeed.h")

lc-cov: clean-gcda (cov-run "tests/lc_test4" INCS) (cov-run "tests/lc_test8" INCS) (cov-show "linecache.h")
pt-cov: clean-gcda (cov-run "tests/pt_test4" INCS) (cov-show "piecetab.h")
ut-cov: clean-gcda (cov-run "tests/ut_test" INCS) (cov-show "undotree.h")
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

# lua bindings & tests (delegate to lua/justfile, cwd = lua/)

lua-run t *args='':
    @just -f lua/justfile {{t}} {{args}}
lua-pt *args='': (lua-run "pt" args)
lua-cg *args='': (lua-run "cg" args)
lua-tf *args='': (lua-run "tf" args)
lua-ed *args='': (lua-run "ed" args)

lua-cov t:
    @just -f lua/justfile cov-{{t}}
lua-pt-cov: (lua-cov "pt")
lua-cg-cov: (lua-cov "cg")
lua-tf-cov: (lua-cov "tf")

lua-lines t:
    @just -f lua/justfile lines {{t}}
lua-pt-lines: (lua-lines "piecetab")
lua-cg-lines: (lua-lines "cellgrid")
lua-tf-lines: (lua-lines "termfeed")

clean: clean-gcda
    rm -f tests/lc_test4 tests/lc_test8 tests/pt_test4 tests/ut_test tests/cellgrid_test tests/termfeed_test
    rm -fr tests/*.dSYM
    rm -fr build
