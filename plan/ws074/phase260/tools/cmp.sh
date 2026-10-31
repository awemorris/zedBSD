#!/bin/sh
# Compares the layout dumps (boxes only) of a reftest and its -ref file:  P=DIR sh cmp.sh NAME [LINES]
#   P defaults to build/ws074-suites/wpt/css/css-grid/subgrid
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
cd "$(dirname -- "$0")/../../../.."
P=${P:-build/ws074-suites/wpt/css/css-grid/subgrid}
L=plan/ws074/phase260/tools/lay.sh
t=$(mktemp); r=$(mktemp)
sh $L "$P/$1.html" | grep -v "^ *text\|^viewport" | sed 's/^ *//' > "$t"
sh $L "$P/$1-ref.html" | grep -v "^ *text\|^viewport" | sed 's/^ *//' > "$r"
diff "$t" "$r" | head -"${2:-40}"
