#!/bin/sh
# Build the production tee and exercise its bounded conformance surface.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-tee-test.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
	-I"$repo" \
	"$repo/userland/base/tee/main.c" \
	"$repo/userland/base/common/command.c" \
	-o "$temporary/tee"
tee=$temporary/tee

printf 'alpha\000beta\n' >"$temporary/input"
"$tee" <"$temporary/input" >"$temporary/stdout"
cmp "$temporary/input" "$temporary/stdout"

"$tee" "$temporary/default" <"$temporary/input" >"$temporary/default.out"
cmp "$temporary/input" "$temporary/default"
cmp "$temporary/input" "$temporary/default.out"

printf 'prefix\n' >"$temporary/append"
"$tee" -a "$temporary/append" <"$temporary/input" >/dev/null
{
	printf 'prefix\n'
	cat "$temporary/input"
} >"$temporary/append.expected"
cmp "$temporary/append.expected" "$temporary/append"

set --
index=0
while test "$index" -lt 40; do
	set -- "$@" "$temporary/many-$index"
	index=$((index + 1))
done
"$tee" "$@" <"$temporary/input" >/dev/null
index=0
while test "$index" -lt 40; do
	cmp "$temporary/input" "$temporary/many-$index"
	index=$((index + 1))
done

set +e
"$tee" "$temporary" "$temporary/open-success" \
	<"$temporary/input" >"$temporary/open.out" 2>"$temporary/open.err"
status=$?
set -e
test "$status" -ne 0
cmp "$temporary/input" "$temporary/open-success"
cmp "$temporary/input" "$temporary/open.out"
test -s "$temporary/open.err"

if test -e /dev/full; then
	set +e
	"$tee" /dev/full "$temporary/write-success" \
		<"$temporary/input" >"$temporary/write.out" 2>"$temporary/write.err"
	status=$?
	set -e
	test "$status" -ne 0
	cmp "$temporary/input" "$temporary/write-success"
	cmp "$temporary/input" "$temporary/write.out"
	test -s "$temporary/write.err"

	set +e
	"$tee" "$temporary/stdout-failure" \
		<"$temporary/input" >/dev/full 2>"$temporary/stdout.err"
	status=$?
	set -e
	test "$status" -ne 0
	cmp "$temporary/input" "$temporary/stdout-failure"
	test -s "$temporary/stdout.err"
fi

(
	cd "$temporary"
	"$tee" -- - <input >/dev/null
	cmp input ./-
)

printf 'one' >"$temporary/duplicate"
printf 'two' | "$tee" -a "$temporary/duplicate" "$temporary/duplicate" \
	>/dev/null
test "$(cat "$temporary/duplicate")" = "onetwotwo"

mkfifo "$temporary/interrupt.fifo"
"$tee" -i "$temporary/interrupt.out" <"$temporary/interrupt.fifo" \
	>"$temporary/interrupt.stdout" &
tee_pid=$!
exec 3>"$temporary/interrupt.fifo"
printf 'before' >&3
sleep 0.05
kill -INT "$tee_pid"
printf 'after' >&3
exec 3>&-
wait "$tee_pid"
test "$(cat "$temporary/interrupt.out")" = "beforeafter"
test "$(cat "$temporary/interrupt.stdout")" = "beforeafter"

set +e
"$tee" "$temporary/read-error" <"$temporary" \
	>/dev/null 2>"$temporary/read.err"
status=$?
set -e
test "$status" -ne 0
test -s "$temporary/read.err"

if "$tee" -z </dev/null >/dev/null 2>&1; then
	echo "TEE-T001 invalid option was accepted" >&2
	exit 1
fi

echo "TEE-T001 options, outputs, failures, binary data, and SIGINT: PASS"
