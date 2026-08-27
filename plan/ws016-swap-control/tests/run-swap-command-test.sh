#!/bin/sh
# SWAP-T009/T010 production-linked command runner.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-swap-command.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

run_variant()
{
	variant=$1
	variant_flags=$2
	common_flags="-std=c11 -D_POSIX_C_SOURCE=200809L -I. -Iinclude/uapi"
	common_flags="$common_flags -Wall -Wextra -Werror"

	# shellcheck disable=SC2086
	"${CC:-cc}" $common_flags $variant_flags \
		-include plan/ws016-swap-control/tests/swap-command-hooks.h \
		-DSWAP_COMMAND_OPEN=swap_test_open \
		-DSWAP_COMMAND_IOCTL=swap_test_ioctl \
		-DSWAP_COMMAND_CLOSE=swap_test_close \
		-Dmain=swapon_program_main \
		-c userland/base/swapon/main.c -o "$temporary/swapon-$variant.o"
	# shellcheck disable=SC2086
	"${CC:-cc}" $common_flags $variant_flags \
		-include plan/ws016-swap-control/tests/swap-command-hooks.h \
		-DSWAP_COMMAND_OPEN=swap_test_open \
		-DSWAP_COMMAND_IOCTL=swap_test_ioctl \
		-DSWAP_COMMAND_CLOSE=swap_test_close \
		-Dmain=swapoff_program_main \
		-c userland/base/swapoff/main.c -o "$temporary/swapoff-$variant.o"
	# shellcheck disable=SC2086
	"${CC:-cc}" $common_flags $variant_flags \
		plan/ws016-swap-control/tests/swap-command-test.c \
		"$temporary/swapon-$variant.o" "$temporary/swapoff-$variant.o" \
		-o "$temporary/swap-command-test-$variant"
	if [ "$variant" = sanitizer ]; then
		ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
			"$temporary/swap-command-test-$variant"
	else
		"$temporary/swap-command-test-$variant"
	fi
}

cd "$repo_dir"
run_variant strict ""
run_variant sanitizer \
	"-g -fno-omit-frame-pointer -fsanitize=address,undefined"
