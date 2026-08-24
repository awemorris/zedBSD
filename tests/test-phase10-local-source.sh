#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"

expected_bc=$'Makefile\nmain.c\nnumber.c\nnumber.h'
expected_ed=$'Makefile\nbuffer.c\neditor.h\nmain.c'
expected_m4=$'Makefile\nengine.c\nm4.h\nmain.c'

check_manifest()
{
	local package="$1"
	local expected="$2"
	local actual

	actual="$(find "$repo/userland/base/$package" -type f \
		-printf '%f\n' | LC_ALL=C sort)"
	if [[ "$actual" != "$expected" ]]; then
		echo "Phase 10 source manifest mismatch for $package" >&2
		diff -u <(printf '%s\n' "$expected") <(printf '%s\n' "$actual") \
			>&2 || true
		exit 1
	fi
}

check_manifest bc "$expected_bc"
check_manifest ed "$expected_ed"
check_manifest m4 "$expected_m4"

if [[ -e "$repo/tests/m4-host-compat.c" || \
	-e "$repo/tests/m4-host-compat.h" ]]; then
	echo 'obsolete m4 host compatibility layer remains' >&2
	exit 1
fi

if rg -n '\$OpenBSD:|Gavin D[.] Howard|Free Software Foundation|SPDX-License-Identifier: BSD-2-Clause' \
	"$repo/userland/base/bc" "$repo/userland/base/ed" \
	"$repo/userland/base/m4"; then
	echo 'an imported implementation fingerprint remains in Phase 10 source' >&2
	exit 1
fi

for source in \
	"$repo/userland/base/bc/main.c" \
	"$repo/userland/base/bc/number.c" \
	"$repo/userland/base/bc/number.h" \
	"$repo/userland/base/ed/main.c" \
	"$repo/userland/base/ed/buffer.c" \
	"$repo/userland/base/ed/editor.h" \
	"$repo/userland/base/m4/main.c" \
	"$repo/userland/base/m4/engine.c" \
	"$repo/userland/base/m4/m4.h"; do
	if ! head -n 1 "$source" | grep -q \
		'^/[*] Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib [*]/$'; then
		echo "missing zedBSD source header: $source" >&2
		exit 1
	fi
done

if rg -n 'userland/base/(bc/(src|include|gen)|ed/(buf|glbl|io|re|sub|undo)[.]c|m4/(eval|expr|gnum4|look|misc|ohash|parser|tokenizer|trace)[.]c)|m4-host-compat' \
	"$repo/Makefile" "$repo/userland/base/bc/Makefile" \
	"$repo/userland/base/ed/Makefile" "$repo/userland/base/m4/Makefile" \
	"$repo/tests/test-posix-bc-host.sh" \
	"$repo/tests/test-posix-ed-host.sh" \
	"$repo/tests/test-posix-m4-host.sh"; then
	echo 'a build or test still references removed imported source' >&2
	exit 1
fi

echo 'zedBSD Phase 10 local source provenance test: PASS'
