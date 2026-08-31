#!/bin/sh
# Build the production cmp and exercise its bounded conformance surface.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-cmp-test.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
	-I"$repo" \
	"$repo/userland/base/cmp/main.c" \
	"$repo/userland/base/common/command.c" \
	-o "$temporary/cmp"
cmp=$temporary/cmp

: >"$temporary/empty-a"
: >"$temporary/empty-b"
"$cmp" "$temporary/empty-a" "$temporary/empty-b"

printf 'first\nsecond\n' >"$temporary/equal-a"
cp "$temporary/equal-a" "$temporary/equal-b"
"$cmp" "$temporary/equal-a" "$temporary/equal-b"

printf 'aX\nZ\n' >"$temporary/different-a"
printf 'aY\nQ\n' >"$temporary/different-b"
set +e
output=$("$cmp" "$temporary/different-a" "$temporary/different-b")
status=$?
set -e
test "$status" -eq 1
test "$output" = "$temporary/different-a $temporary/different-b differ: char 2, line 1"

set +e
output=$("$cmp" -l "$temporary/different-a" "$temporary/different-b")
status=$?
set -e
test "$status" -eq 1
test "$output" = "2 130 131
4 132 121"

set +e
output=$("$cmp" -s "$temporary/different-a" "$temporary/different-b" 2>&1)
status=$?
set -e
test "$status" -eq 1
test -z "$output"

printf 'short' >"$temporary/short"
printf 'shorter' >"$temporary/long"
set +e
"$cmp" "$temporary/short" "$temporary/long" \
	>"$temporary/eof.out" 2>"$temporary/eof.err"
status=$?
set -e
test "$status" -eq 1
test ! -s "$temporary/eof.out"
test "$(cat "$temporary/eof.err")" = "cmp: EOF on $temporary/short"

printf 'xxpayload' >"$temporary/skip-a"
printf 'payload' >"$temporary/skip-b"
"$cmp" "$temporary/skip-a" "$temporary/skip-b" 2
"$cmp" "$temporary/skip-a" "$temporary/skip-b" 02 0

for value in 09 -1 +1 18446744073709551616 invalid; do
	if "$cmp" "$temporary/equal-a" "$temporary/equal-b" "$value" \
	    >/dev/null 2>&1; then
		echo "CMP-T001 invalid skip accepted: $value" >&2
		exit 1
	fi
done

cp "$temporary/equal-a" "$temporary/-left"
cp "$temporary/equal-a" "$temporary/-right"
(
	cd "$temporary"
	"$cmp" -- -left -right
)
if "$cmp" -l -s "$temporary/equal-a" "$temporary/equal-b" \
    >/dev/null 2>&1; then
	echo "CMP-T001 mutually exclusive modes were accepted" >&2
	exit 1
fi
if "$cmp" - - </dev/null >/dev/null 2>&1; then
	echo "CMP-T001 both-standard-input operands were accepted" >&2
	exit 1
fi
if "$cmp" "$temporary/missing" "$temporary/equal-b" \
    >/dev/null 2>&1; then
	echo "CMP-T001 missing input was accepted" >&2
	exit 1
fi

mkfifo "$temporary/fifo-a" "$temporary/fifo-b"
(
	printf 'abc'
	sleep 0.05
	printf 'def'
) >"$temporary/fifo-a" &
first_writer=$!
(
	printf 'a'
	sleep 0.01
	printf 'bcdef'
) >"$temporary/fifo-b" &
second_writer=$!
"$cmp" "$temporary/fifo-a" "$temporary/fifo-b"
wait "$first_writer"
wait "$second_writer"

echo "CMP-T001 options, offsets, status, EOF, and short reads: PASS"
