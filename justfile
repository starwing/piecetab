import 'build.just'

# C tests: one runner per lib, parametrized. Lua side lives in lua/justfile.

INCS := "-I. -Itests"

test t *args='': (dbg-run t INCS args)

lc4 *args='': (test "tests/linecache_test_fanout4" args)
lc8 *args='': (test "tests/linecache_test_fanout8" args)
lc *args='': (test "tests/linecache_test_fanout4" "?" + args) (test "tests/linecache_test_fanout8" "?" + args)
pt *args='': (test "tests/piecetab_test_fanout4" args)
ut *args='': (test "tests/undotree_test" args)
cg *args='': (test "tests/cellgrid_test" args)
tf *args='': (test "tests/termfeed_test" args)
sp4 *args='': (test "tests/spantree_test_fanout4" args)
sp8 *args='': (test "tests/spantree_test_fanout8" args)
sp *args='': (test "tests/spantree_test_fanout4" "?" + args) (test "tests/spantree_test_fanout8" "?" + args)

# fuzz: seeded random-op stress with per-op invariant checks
sp-fuzz seed='1': (fuzz-run "sp" seed)
pt-fuzz seed='1': (fuzz-run "pt" seed)
lc-fuzz seed='1': (fuzz-run "lc" seed)
fuzz-replay lib path: (fuzz-run lib "replay" path)

# debug fuzz: ASan/UBSan build, precise fault isolation
dfz-sp seed='1': (dfz-run "sp" seed)
dfz-pt seed='1': (dfz-run "pt" seed)
dfz-lc seed='1': (dfz-run "lc" seed)

# coverage

cov: clean-gcda (cov-run "tests/linecache_test_fanout4" INCS) (cov-run "tests/linecache_test_fanout8" INCS) (cov-run "tests/piecetab_test_fanout4" INCS) (cov-run "tests/undotree_test" INCS) (cov-run "tests/cellgrid_test" INCS) (cov-run "tests/termfeed_test" INCS) (cov-run "tests/spantree_test_fanout4" INCS) (cov-show "piecetab.h linecache.h undotree.h cellgrid.h termfeed.h spantree.h")

lc-cov: clean-gcda (cov-run "tests/linecache_test_fanout4" INCS) (cov-run "tests/linecache_test_fanout8" INCS) (cov-show "linecache.h")
pt-cov: clean-gcda (cov-run "tests/piecetab_test_fanout4" INCS) (cov-show "piecetab.h")
ut-cov: clean-gcda (cov-run "tests/undotree_test" INCS) (cov-show "undotree.h")
cg-cov: clean-gcda (cov-run "tests/cellgrid_test" INCS) (cov-show "cellgrid.h")
tf-cov: clean-gcda (cov-run "tests/termfeed_test" INCS) (cov-show "termfeed.h")
sp-cov: clean-gcda (cov-run "tests/spantree_test_fanout4" INCS) (cov-run "tests/spantree_test_fanout8" INCS) (cov-show "spantree.h")

lc-lines: (cov-lines "linecache.h")
pt-lines: (cov-lines "piecetab.h")
ut-lines: (cov-lines "undotree.h")
cg-lines: (cov-lines "cellgrid.h")
tf-lines: (cov-lines "termfeed.h")
sp-lines: (cov-lines "spantree.h")

lc-unbranched: (cov-unbranched "linecache.h")
pt-unbranched: (cov-unbranched "piecetab.h")
ut-unbranched: (cov-unbranched "undotree.h")
cg-unbranched: (cov-unbranched "cellgrid.h")
tf-unbranched: (cov-unbranched "termfeed.h")
sp-unbranched: (cov-unbranched "spantree.h")

# lua bindings & tests live in lua/justfile — run with: just lua/<recipe>

# capture an SVG screenshot of the editor demo (needs tmux + ansisvg;
# assumes the Lua modules are already built)
# example: just svg                    -> misc/demo.svg of `lua editor.lua editor.lua`
#          just svg README.md          -> misc/demo.svg of `lua editor.lua README.md`
# customize: just SVG_OUT=foo.svg SVG_COLS=120 SVG_ROWS=36 SVG_WAIT=5 svg README.md
SVG_OUT := 'misc/demo.svg'
SVG_COLS := '90'
SVG_ROWS := '24'
SVG_WAIT := '5'
svg *args='editor.lua':
    tmux kill-session -t shot 2>/dev/null || true
    tmux new-session -d -s shot -x {{ SVG_COLS }} -y {{ SVG_ROWS }} 'lua editor.lua {{ args }}'
    sleep {{ SVG_WAIT }}
    tmux capture-pane -t shot -e -p > /tmp/piecetab-editor.ansi
    tmux kill-session -t shot
    ansisvg --width {{ SVG_COLS }} < /tmp/piecetab-editor.ansi > {{ SVG_OUT }}

clean: clean-gcda
    rm -f tests/linecache_test_fanout4 tests/linecache_test_fanout8 tests/piecetab_test_fanout4 tests/undotree_test tests/cellgrid_test tests/termfeed_test tests/spantree_test_fanout4 tests/spantree_test_fanout8
    rm -fr tests/*.dSYM
    rm -fr build

# LuaLS type check on lua sources (target: zero warnings in lsp.lua/editor.lua/tests)

luals:
    rm -rf /tmp/luals_check
    mkdir -p /tmp/luals_check/log
    lua-language-server --check . --checklevel=Warning --configpath=.luarc.json --logpath=/tmp/luals_check/log
