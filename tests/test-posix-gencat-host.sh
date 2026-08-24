#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# POSIX-UTILITY-TEST: gencat positive negative
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-posix-gencat.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -I"$repo" \
	"$repo/userland/base/gencat/main.c" \
	"$repo/userland/base/common/command.c" -o "$work/gencat"
cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -I"$repo" \
	"$repo/libc/catalog.c" \
	"$repo/tests/posix-gencat-host-test.c" -o "$work/catalog-test"

cat >"$work/messages.msg" <<'EOF'
$quote "
$set 2
1 "hello"
2 "line\nnext"
$set 3
1 old
EOF
"$work/gencat" "$work/messages.cat" "$work/messages.msg"
"$work/catalog-test" "$work/messages.cat"

cp "$work/messages.cat" "$work/before.cat"
cat >"$work/invalid.msg" <<'EOF'
$set invalid
EOF
if "$work/gencat" "$work/messages.cat" "$work/invalid.msg" 2>/dev/null; then
	echo 'gencat accepted an invalid set number' >&2
	exit 1
fi
cmp "$work/messages.cat" "$work/before.cat"

cat >"$work/update.msg" <<'EOF'
$quote "
$set 2
1 "updated"
2
$delset 3
EOF
"$work/gencat" "$work/messages.cat" "$work/update.msg"

cat >"$work/check-update.c" <<'EOF'
#include <errno.h>
#include "libc/include/nl_types.h"
#include <stdlib.h>
#include <string.h>
int main(void) {
    nl_catd c = catopen("messages", 0);
    const char *f = "missing";
    int ok = c != (nl_catd)-1 && !strcmp(catgets(c, 2, 1, f), "updated");
    errno = 0;
    ok = ok && catgets(c, 2, 2, f) == f && errno == ENOMSG;
    ok = ok && catgets(c, 3, 1, f) == f && errno == ENOMSG;
    return !ok || catclose(c) != 0;
}
EOF
cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -I"$repo" \
	"$repo/libc/catalog.c" \
	"$work/check-update.c" -o "$work/check-update"
NLSPATH="$work/%N.cat" LANG=C "$work/check-update"
mkdir -p "$work/nls/zz_ZZ.UTF-8" "$work/nls/zz/ZZ/UTF-8"
cp "$work/messages.cat" "$work/nls/zz_ZZ.UTF-8/messages.cat"
cp "$work/messages.cat" "$work/nls/zz/ZZ/UTF-8/messages.cat"
NLSPATH="$work/nls/%L/%N.cat" LANG=zz_ZZ.UTF-8 \
	"$work/check-update"
NLSPATH="$work/nls/%l/%t/%c/%N.cat" LANG=zz_ZZ.UTF-8 \
	"$work/check-update"

printf 'not a catalog' >"$work/corrupt.cat"
if "$work/catalog-test" "$work/corrupt.cat" 2>/dev/null; then
	echo 'catopen accepted a corrupt catalog' >&2
	exit 1
fi

echo 'zedBSD POSIX gencat/catalog host tests: PASS'
