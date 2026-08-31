#!/bin/sh
# Build and run the production shared-command host fixture.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-base-command.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
	-I"$repo" \
	"$repo/userland/base/common/command.c" \
	"$repo/plan/ws001-posix/tests/base-command-host-test.c" \
	-o "$temporary/base-command-host-test"
"$temporary/base-command-host-test"

echo "BASE-STYLE-T003 shared command behavior: PASS"
