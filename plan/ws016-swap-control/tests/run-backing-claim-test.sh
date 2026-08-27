#!/bin/sh
set -eu

repo=${1:-.}
out=${TMPDIR:-/tmp}/zedbsd-backing-claim-test.$$
trap 'rm -f "$out"' EXIT HUP INT TERM

cc -std=c11 -I"$repo/include" -I"$repo/include/uapi" -I"$repo" \
	-Wall -Wextra -Werror -ffunction-sections -fdata-sections \
	"$repo/src/kern/backing-claim.c" \
	"$repo/plan/ws016-swap-control/tests/backing-claim-test.c" \
	-Wl,--gc-sections -pthread -o "$out"
"$out"
