#!/usr/bin/env bash
# WS012 SVC-T006 consolidated host regression acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
compiler=${CC:-cc}

if [[ $# -ne 1 ]]; then
	echo "usage: $0 OUTPUT-DIRECTORY" >&2
	exit 2
fi
output=$1
if [[ -e $output ]]; then
	echo "output path already exists: $output" >&2
	exit 2
fi
command -v "$compiler" >/dev/null
mkdir -p -- "$output"
output=$(cd -- "$output" && pwd)

common=(-std=c17 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
	-I"$repo" -Wall -Wextra -Werror)
sanitizers=(-g -fno-omit-frame-pointer -fsanitize=address,undefined)
tests=$repo/plan/ws012-service-console/tests
service=$repo/userland/base/service
results=$output/results.tsv
printf 'case\tvariant\tresult\tevidence\n' >"$results"

run_case()
{
	local name=$1 variant=$2
	shift 2
	local binary=$output/$name-$variant
	local log=$output/$name-$variant.log
	local -a variant_flags=()

	if [[ $variant == sanitizer ]]; then
		variant_flags=("${sanitizers[@]}")
	fi
	{
		printf 'compile:'
		printf ' %q' "$compiler" "${common[@]}" \
		    "${variant_flags[@]}" "$@" -o "$binary"
		printf '\n'
		"$compiler" "${common[@]}" "${variant_flags[@]}" "$@" \
		    -o "$binary"
		printf 'run: %q\n' "$binary"
		if [[ $variant == sanitizer ]]; then
			ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
			    "$binary"
		else
			"$binary"
		fi
	} >"$log" 2>&1
	printf '%s\t%s\tpass\t%s\n' "$name" "$variant" \
	    "$(basename -- "$log")" >>"$results"
}

run_persistence()
{
	local variant=$1
	local object=$output/rcconf-persistence-$variant.o
	local binary=$output/rcconf-persistence-$variant
	local log=$output/rcconf-persistence-$variant.log
	local -a variant_flags=()
	local -a hooks=(-include "$tests/rcconf-persistence-hooks.h"
		-Dwrite=rcconf_test_write -Dfsync=rcconf_test_fsync
		-Drename=rcconf_test_rename -Dunlink=rcconf_test_unlink)

	if [[ $variant == sanitizer ]]; then
		variant_flags=("${sanitizers[@]}")
	fi
	{
		printf 'compile-object:'
		printf ' %q' "$compiler" "${common[@]}" \
		    "${variant_flags[@]}" "${hooks[@]}" -c "$service/rcconf.c" \
		    -o "$object"
		printf '\n'
		"$compiler" "${common[@]}" "${variant_flags[@]}" \
		    "${hooks[@]}" -c "$service/rcconf.c" -o "$object"
		printf 'compile-test:'
		printf ' %q' "$compiler" "${common[@]}" \
		    "${variant_flags[@]}" "$tests/rcconf-persistence-test.c" \
		    "$service/service-config.c" "$object" -o "$binary"
		printf '\n'
		"$compiler" "${common[@]}" "${variant_flags[@]}" \
		    "$tests/rcconf-persistence-test.c" \
		    "$service/service-config.c" "$object" -o "$binary"
		printf 'run: %q\n' "$binary"
		if [[ $variant == sanitizer ]]; then
			ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
			    "$binary"
		else
			"$binary"
		fi
	} >"$log" 2>&1
	printf 'rcconf-persistence\t%s\tpass\t%s\n' "$variant" \
	    "$(basename -- "$log")" >>"$results"
}

for variant in strict sanitizer; do
	run_case rcconf-model "$variant" \
	    "$tests/rcconf-model-test.c" "$service/rcconf.c" \
	    "$service/service-config.c"
	run_persistence "$variant"
	run_case zsv1-protocol "$variant" \
	    "$tests/zsv1-protocol-test.c" "$service/zsv1-protocol.c"
	run_case zsv1-client "$variant" \
	    "$tests/zsv1-client-test.c" "$service/zsv1-client.c" \
	    "$service/zsv1-protocol.c"
	run_case zsv1-server "$variant" \
	    "$tests/zsv1-server-test.c" "$service/zsv1-server.c" \
	    "$service/zsv1-protocol.c"
	run_case zsv1-shutdown-argv "$variant" \
	    "$tests/zsv1-shutdown-argv-test.c"
	run_case service-command "$variant" \
	    "$tests/service-command-test.c" "$service/service-command.c" \
	    "$service/rcconf.c" "$service/service-config.c" \
	    "$service/zsv1-client.c" "$service/zsv1-protocol.c"
	run_case service-console "$variant" \
	    "$tests/service-console-test.c" "$service/service-console.c" \
	    "$service/service-command.c" "$service/rcconf.c" \
	    "$service/service-config.c" "$service/zsv1-client.c" \
	    "$service/zsv1-protocol.c"
done

echo "WS012 host service acceptance: PASS ($output)"
