#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-phase6.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -I"$repo" \
	"$repo/userland/base/ar/main.c" \
	"$repo/userland/base/common/archive.c" \
	"$repo/userland/base/common/elf_symbols.c" -o "$work/ar"
cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -I"$repo" \
	"$repo/userland/base/nm/main.c" \
	"$repo/userland/base/common/archive.c" \
	"$repo/userland/base/common/elf_symbols.c" -o "$work/nm"
cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -I"$repo" \
	"$repo/userland/base/cflow/main.c" \
	"$repo/userland/base/common/c_parser.c" -o "$work/cflow"
cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -I"$repo" \
	"$repo/userland/base/cxref/main.c" \
	"$repo/userland/base/common/c_parser.c" -o "$work/cxref"

printf 'first\n' >"$work/one.txt"
printf 'long\n' >"$work/a-member-name-longer-than-fifteen.txt"
"$work/ar" -rc "$work/libtest.a" "$work/one.txt" \
	"$work/a-member-name-longer-than-fifteen.txt"
"$work/ar" -t "$work/libtest.a" >"$work/list"
grep -qx one.txt "$work/list"
grep -qx a-member-name-longer-than-fifteen.txt "$work/list"
printf 'second\n' >"$work/one.txt"
"$work/ar" -r "$work/libtest.a" "$work/one.txt"
mkdir "$work/extract"
(cd "$work/extract" && "$work/ar" -x ../libtest.a one.txt)
grep -qx second "$work/extract/one.txt"
"$work/ar" -d "$work/libtest.a" one.txt
if "$work/ar" -t "$work/libtest.a" | grep -q '^one.txt$'; then
	exit 1
fi

cc -c "$repo/tests/fixtures/phase6-cflow.c" -o "$work/phase6.o"
"$work/nm" -P "$work/phase6.o" >"$work/nm.out"
grep -Eq '^main [Tt] ' "$work/nm.out"
grep -Eq '^helper [Tt] ' "$work/nm.out"
"$work/ar" -q "$work/libobj.a" "$work/phase6.o"
nm --print-armap "$work/libobj.a" >"$work/armap.out"
grep -q 'main in phase6.o' "$work/armap.out"
"$work/nm" -A "$work/libobj.a" >"$work/nm-ar.out"
grep -q 'libobj.a(phase6.o).* main$' "$work/nm-ar.out"

printf 'two\n' >"$work/two.txt"
printf 'three\n' >"$work/three.txt"
"$work/ar" -q "$work/move.a" "$work/one.txt" "$work/two.txt" \
	"$work/three.txt"
"$work/ar" -mb one.txt "$work/move.a" three.txt
test "$("$work/ar" -t "$work/move.a" | tr '\n' ' ')" = \
	'three.txt one.txt two.txt '

"$work/cflow" "$repo/tests/fixtures/phase6-cflow.c" >"$work/cflow.out"
grep -q '^main ' "$work/cflow.out"
grep -q '    helper ' "$work/cflow.out"
if grep -q not_a_call "$work/cflow.out"; then
	exit 1
fi
"$work/cxref" "$repo/tests/fixtures/phase6-cflow.c" >"$work/cxref.out"
grep -q '^main ' "$work/cxref.out"
grep -q '^helper ' "$work/cxref.out"

printf '%s\n' 'zedBSD POSIX Phase 6 development host test: PASS'
