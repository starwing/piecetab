#!/bin/sh
# fetch_grammars.sh -- download tree-sitter grammars and build .so files
# usage: fetch_grammars.sh <lang> [<lang>...]
#   lang[@owner/repo] -- @repo overrides the default owner/repo
#   default: tree-sitter/tree-sitter-<lang> (lua -> MunifTanjim/tree-sitter-lua,
#   the official repo was removed)
set -e
here=$(cd "$(dirname "$0")" && pwd)
out="$here/../lua/grammar"
mkdir -p "$out"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

for spec in "$@"; do
  lang=${spec%%@*}
  case "$spec" in *@*) repo=${spec#*@} ;;
    lua) repo="MunifTanjim/tree-sitter-lua" ;;
    *) repo="tree-sitter/tree-sitter-$lang" ;;
  esac
  echo "== $lang ($repo) =="
  curl -fsSL "https://github.com/$repo/archive/refs/heads/master.tar.gz" \
    -o "$tmp/$lang.tar.gz" || \
  curl -fsSL "https://github.com/$repo/archive/refs/heads/main.tar.gz" \
    -o "$tmp/$lang.tar.gz"
  tar xzf "$tmp/$lang.tar.gz" -C "$tmp"
  srcdir=$(find "$tmp" -maxdepth 1 -type d -name "*$lang-*" | head -1)
  scanner=""
  if [ -f "$srcdir/src/scanner.c" ]; then scanner="$srcdir/src/scanner.c"; fi
  ${CC:-cc} -O2 -fPIC -shared -I"$srcdir/src" \
    -o "$out/tree_sitter_$lang.so" "$srcdir/src/parser.c" $scanner
  echo "  -> $out/tree_sitter_$lang.so"
done
