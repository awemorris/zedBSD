#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/ws003-p025-config-stamp.XXXXXX")

cleanup()
{
	rm -rf "$work"
}
trap cleanup 0 HUP INT TERM

build=$work/build
first_log=$work/first.log
changed_log=$work/changed.log
stable_log=$work/stable.log
object=$build/src/hal/amd64/bsp-pcat/timecounter.o
first_object=$work/timecounter-first.o

cd "$repo"

make --no-print-directory -j16 \
	ZEDBSD_CONFIG=config/ci/config-amd64.mk \
	BUILD="$build" \
	ZEDBSD_TEST_CPPFLAGS=-DZEDBSD_TEST_TIMECOUNTER_INCONSISTENT_CPU \
	vmunix >"$first_log" 2>&1
test -f "$object"
cp "$object" "$first_object"

# Reuse the same BUILD directory while changing only the private injection.
# The platform-config stamp must invalidate every object compiled with the
# effective configuration, including the timecounter implementation itself.
make --no-print-directory -j16 \
	ZEDBSD_CONFIG=config/ci/config-amd64.mk \
	BUILD="$build" \
	ZEDBSD_TEST_CPPFLAGS=-DZEDBSD_TEST_TIMECOUNTER_INCONSISTENT_SAMPLE \
	vmunix >"$changed_log" 2>&1

grep -F -- '-DZEDBSD_TEST_TIMECOUNTER_INCONSISTENT_SAMPLE' "$changed_log" \
	>/dev/null
grep -F -- '-c src/hal/amd64/bsp-pcat/timecounter.c -o' "$changed_log" \
	>/dev/null
if cmp -s "$first_object" "$object"; then
	echo "timecounter object did not change with ZEDBSD_TEST_CPPFLAGS" >&2
	exit 1
fi
grep -F -- '-DZEDBSD_TEST_TIMECOUNTER_INCONSISTENT_SAMPLE' \
	"$build/.platform-config" >/dev/null
if grep -F -- '-DZEDBSD_TEST_TIMECOUNTER_INCONSISTENT_CPU' \
	"$build/.platform-config" >/dev/null; then
	echo "platform-config stamp retained the previous injection" >&2
	exit 1
fi

# A third invocation with unchanged flags must leave the object up to date.
make --no-print-directory -j16 \
	ZEDBSD_CONFIG=config/ci/config-amd64.mk \
	BUILD="$build" \
	ZEDBSD_TEST_CPPFLAGS=-DZEDBSD_TEST_TIMECOUNTER_INCONSISTENT_SAMPLE \
	vmunix >"$stable_log" 2>&1
if grep -F -- '-c src/hal/amd64/bsp-pcat/timecounter.c -o' "$stable_log" \
	>/dev/null; then
	echo "timecounter object rebuilt although the configuration was stable" >&2
	exit 1
fi

echo "WS003 P025 platform-config rebuild: PASS"
