#!/bin/sh
set -eu

cc=${CC:-cc}
output=${TMPDIR:-/tmp}/ws003-br-t52-early-init-policy

cleanup()
{
	rm -f "$output"
}
trap cleanup 0 HUP INT TERM

"$cc" -std=c11 -Iinclude -I. -Wall -Wextra -Werror \
	src/hal/amd64/bsp-pcat/early-init-policy.c \
	plan/ws003-bringup/tests/early-init-policy-test.c \
	-o "$output"
"$output"
