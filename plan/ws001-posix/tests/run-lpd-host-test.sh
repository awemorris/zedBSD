#!/bin/sh
# Build the production lp/lpr client and run the fake LPD protocol matrix.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-lpd-build.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
	-I"$repo" \
	"$repo/userland/base/lp/main.c" \
	"$repo/userland/base/lp/lpd-client.c" \
	"$repo/userland/base/common/command.c" \
	-o "$temporary/lp"
ln -s lp "$temporary/lpr"

python3 "$repo/plan/ws001-posix/tests/fake-lpd-test.py" \
	"$temporary/lp" "$temporary/lpr"
