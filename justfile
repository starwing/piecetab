import 'build.just'

mod t 'tests/justfile'
mod f 'fuzz/justfile'
mod l 'lua/justfile'
mod b 'bench/justfile'

# C tests: one runner per lib, parametrized. Lua side lives in lua/justfile.

alias lc4 := t::lc4
alias lc8 := t::lc8
alias lc := t::lc
alias pt := t::pt
alias sp4 := t::sp4
alias sp8 := t::sp8
alias sp := t::sp
alias ut := t::ut
alias cg := t::cg
alias tf := t::tf
alias tm := t::tm
alias cov := t::cov

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

# remove coverage artifacts in all subprojects
clean-gcda: l::clean-gcda t::clean-gcda (rm-gcda ".") (rm-gcda "bench") (rm-gcda "fuzz")

# remove all generated files (C tests, fuzz binaries, Lua modules, bench outputs)
clean: clean-gcda l::clean t::clean f::clean b::clean
    -rm -fr fuzz/*.dSYM
    -rm -f lua/editor.log *.obj
    -rm -f editor.log

# LuaLS type check on lua sources (target: zero warnings in lsp.lua/editor.lua/tests)
luals:
    rm -rf /tmp/luals_check
    mkdir -p /tmp/luals_check/log
    lua-language-server --check . --checklevel=Warning --configpath=.luarc.json --logpath=/tmp/luals_check/log
