#!/bin/sh
# SWAP-T008: production swap-control facade with deterministic resolver hooks.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cc=${CC:-cc}
output=${TMPDIR:-/tmp}/zedbsd-swap-control-test

"$cc" -std=c11 -I"$repo/include" -I"$repo/include/uapi" -I"$repo" \
	-Wall -Wextra -Werror \
	"$repo/src/kern/swap-control.c" \
	"$repo/plan/ws016-swap-control/tests/swap-control-test.c" \
	-o "$output"
"$output"
