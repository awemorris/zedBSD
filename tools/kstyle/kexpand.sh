#!/bin/bash
# kexpand.sh SRC DST : expand compressed lines (one-line bodies, multi-statement
# lines, if/else + statement on one line) of SRC into DST using the repository
# .clang-format, touching only those lines.
set -u
cd /home/awe/claude/zedBSD
CF=build/clangformat-deps/root/usr/bin/clang-format-19
src=$1; dst=$2
heads=$(awk 'NR>1 && $0=="{" && prev ~ /\)[ \t]*$/ {print NR-1} {prev=$0}' "$src" | sed -E 's/^([0-9]+)$/--lines=\1:\1/')
ranges=$(grep -nE '^\{ .*\}\s*$|^\s*[^/*#].*;\s*[a-zA-Z_(*].*;\s*$|^\s*(} else |else |if \(.*\)|while \(.*\)|for \(.*\)|do) *[a-z_]+.*;\s*$|^\s*\} *else *\{ *[a-z].*$|^.*\{ *[a-z_][^}]*; *\}\s*$' "$src" | grep -vE '^[0-9]+:\s*for \([^;]*;[^;]*;[^)]*\)\s*$' | cut -d: -f1 | sed -E 's/^([0-9]+)$/--lines=\1:\1/')
ranges="$ranges $heads"
if [ -z "${ranges// /}" ]; then cp "$src" "$dst"; exit 0; fi
# shellcheck disable=SC2086
$CF --style=file:.clang-format $ranges "$src" > "$dst"
