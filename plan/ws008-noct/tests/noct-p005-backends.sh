#!/usr/bin/env bash
# WS008 NOCT-T040/T043 independent BeUI backend acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
wrapper=$repo/userland/noct
source_dir=$wrapper/NoctLang
pin_file=$wrapper/Makefile
build_dir=$source_dir/build-zedbsd
artifact=$build_dir/noct
library=$build_dir/libnoctapi.a
link_file=$build_dir/CMakeFiles/noctapi.dir/link.txt

if [[ $# -gt 1 ]]; then
	echo "usage: $0 [OUTPUT-DIRECTORY]" >&2
	exit 2
fi
for command in awk cmake git make nm rg sort; do
	command -v "$command" >/dev/null || {
		echo "required command not found: $command" >&2
		exit 2
	}
done
[[ -f $pin_file && -d $source_dir/.git ]] || {
	echo "pinned canonical Noct checkout is missing: $source_dir" >&2
	exit 2
}

if [[ $# -eq 1 ]]; then
	output=$1
	[[ ! -e $output ]] || {
		echo "output path already exists: $output" >&2
		exit 2
	}
	mkdir -p -- "$output"
else
	mkdir -p -- "$repo/plan/ws008-noct/temp"
	output=$(mktemp -d "$repo/plan/ws008-noct/temp/q023-p005-backends.XXXXXX")
fi
output=$(cd -- "$output" && pwd)

expected_revision=$(awk '
	$1 == "NOCT_REVISION" && $2 == "?=" { print $3 }
' "$pin_file")
actual_revision=$(git -C "$source_dir" rev-parse HEAD)
[[ -n $expected_revision && $actual_revision == "$expected_revision" ]] || {
	echo "canonical checkout does not match the zedBSD pin" >&2
	echo "expected: ${expected_revision:-<missing>}" >&2
	echo "actual:   $actual_revision" >&2
	exit 1
}
[[ -z $(git -C "$source_dir" symbolic-ref -q HEAD || true) ]] || {
	echo "canonical checkout is not detached at its pinned revision" >&2
	exit 1
}
[[ -z $(git -C "$source_dir" status --porcelain=v1 --untracked-files=no) ]] || {
	echo "canonical checkout has tracked local changes" >&2
	exit 1
}

public_header=$source_dir/include/noct/beui.h
private_header=$source_dir/src/api/beui-internal.h
[[ -f $public_header && -f $private_header ]] || {
	echo "BeUI public/private headers are incomplete" >&2
	exit 1
}
[[ $(rg -c '^bool noct_register_api_beui\(NoctEnv \*env\);$' \
	"$public_header" || true) -eq 1 ]] || {
	echo "public BeUI header must declare one unsuffixed registrar" >&2
	exit 1
}
if rg -n 'noct_register_api_beui_|noct_beui_(bind|init|image_|bmp_)|struct noct_beui_|enum noct_beui_' \
	"$public_header"; then
	echo "private or suffixed BeUI interface leaked into the public header" >&2
	exit 1
fi
rg -q '^int noct_beui_bind\(const struct noct_beui_hal \*hal\);$' \
	"$private_header"
if rg -n 'noct_register_api_beui_(with_hal|zedbsd|sdl2|pc98dos)' \
	"$source_dir/include" "$source_dir/src" "$source_dir/docs"; then
	echo "obsolete public or platform-suffixed BeUI registrar remains" >&2
	exit 1
fi
for caller in cli-run.c cli-repl.c; do
	rg -q '#include <noct/beui.h>' "$source_dir/src/cli/$caller"
	rg -q 'noct_register_api_beui\(env\)' "$source_dir/src/cli/$caller"
	if rg -q 'beui-internal.h' "$source_dir/src/cli/$caller"; then
		echo "CLI includes the private BeUI contract: $caller" >&2
		exit 1
	fi
done

[[ ! -e $source_dir/src/api/api-beui.c ]]
[[ ! -e $source_dir/src/api/api-beui-backend.c ]]
platform_sources=(
	api-beui-pc98dos.c
	api-beui-sdl2.c
	api-beui-zedbsd.c
)
for source in "${platform_sources[@]}"; do
	path=$source_dir/src/api/$source
	[[ -f $path ]] || { echo "platform BeUI source missing: $source" >&2; exit 1; }
	[[ $(rg -c '^noct_register_api_beui\(NoctEnv \*env\)$' "$path" || true) -eq 1 ]] || {
		echo "platform must own one unsuffixed registrar: $source" >&2
		exit 1
	}
done
rg -q 'NOCT_BEUI_PLATFORM_COUNT EQUAL 1' "$source_dir/CMakeLists.txt"

{
	printf 'tests=NOCT-T040,NOCT-T043\n'
	printf 'repository=%s\n' "$repo"
	printf 'canonical_source=%s\n' "$source_dir"
	printf 'expected_revision=%s\n' "$expected_revision"
	printf 'actual_revision=%s\n' "$actual_revision"
} >"$output/metadata.txt"

make -C "$wrapper" -j16 >"$output/zedbsd-build.log" 2>&1
(cd "$source_dir" && tests/testcases/run-beui-zedbsd.sh "$repo") \
	>"$output/zedbsd-wiring.log" 2>&1

for required in "$artifact" "$library" "$link_file"; do
	[[ -f $required ]] || {
		echo "required zedBSD build artifact missing: $required" >&2
		exit 1
	}
done
selected_sources=$(rg -o 'api-beui-[[:alnum:]-]+\.c\.obj' "$link_file" | sort -u)
[[ $selected_sources == api-beui-zedbsd.c.obj ]] || {
	echo "zedBSD build did not select exactly its platform BeUI source" >&2
	printf '%s\n' "$selected_sources" >&2
	exit 1
}

audit_registrar_symbols()
{
	local input=$1 label=$2 symbols
	symbols=$output/$label-beui-registrars.txt
	nm -g --defined-only "$input" >"$output/$label-global-symbols.txt"
	awk '$NF ~ /^noct_register_api_beui/ { print $NF }' \
		"$output/$label-global-symbols.txt" >"$symbols"
	[[ $(awk 'END { print NR + 0 }' "$symbols") -eq 1 &&
	   $(<"$symbols") == noct_register_api_beui ]] || {
		echo "$label must define exactly the unsuffixed BeUI registrar" >&2
		cat "$symbols" >&2
		exit 1
	}
}

audit_registrar_symbols "$library" library
audit_registrar_symbols "$artifact" executable
[[ -z $(git -C "$source_dir" status --porcelain=v1 --untracked-files=no) ]] || {
	echo "canonical build changed tracked source files" >&2
	exit 1
}

{
	printf 'test\tresult\tevidence\n'
	printf 'NOCT-T040\tpass\tsource audit, link.txt, *-beui-registrars.txt\n'
	printf 'NOCT-T043\tpass\tzedbsd-build.log, zedbsd-wiring.log\n'
} >"$output/results.tsv"
echo "WS008 NOCT-T040/T043 acceptance: PASS ($output)"
