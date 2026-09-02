#!/usr/bin/env bash
# WS008 NOCT-T080--T082/T085 host --path compatibility acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
fixture_rel=plan/ws008-noct/tests/noct-host-path
source_dir=$repo/build/NoctLang
noct=${NOCT:-$source_dir/build-static/noct}
failures=0

if [[ $# -gt 1 ]]; then
	echo "usage: $0 [OUTPUT-DIRECTORY]" >&2
	exit 2
fi
for command in awk cmp git make rg sha256sum tr wc; do
	command -v "$command" >/dev/null || {
		echo "required command not found: $command" >&2
		exit 2
	}
done
[[ -x $noct && -d $source_dir && ! -e $source_dir/.git ]] || {
	echo "release-archive host Noct is not built: $noct" >&2
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
	output=$(mktemp -d "$repo/plan/ws008-noct/temp/p010-path.XXXXXX")
fi
output=$(cd -- "$output" && pwd)

version_file=$repo/userland/base/noct/version.mk
read_release_value() {
	awk -v name="$1" '$1 == "override" && $2 == name && $3 == ":=" { print $4 }' \
		"$version_file"
}
expected_version=$(read_release_value ZEDBSD_NOCT_VERSION)
expected_tag=$(read_release_value ZEDBSD_NOCT_TAG)
expected_revision=$(read_release_value ZEDBSD_NOCT_TAG_COMMIT)
expected_size=$(read_release_value ZEDBSD_NOCT_ARCHIVE_SIZE)
expected_hash=$(read_release_value ZEDBSD_NOCT_ARCHIVE_SHA256)
expected_patch=$(read_release_value ZEDBSD_NOCT_PATCH_LEVEL)
archive=$repo/userland/base/noct/distfiles/NoctLang-$expected_version.tar.gz
identity=$source_dir/.zedbsd-source-identity

[[ $expected_version == 2.0.1 && $expected_tag == v2.0.1 &&
   $expected_revision =~ ^[0-9a-f]{40}$ &&
   $expected_hash =~ ^[0-9a-f]{64}$ && -f $archive && ! -L $archive ]] || {
	echo "host release identity is incomplete" >&2
	exit 1
}
[[ $(wc -c <"$archive" | tr -d '[:space:]') == "$expected_size" &&
   $(sha256sum "$archive" | awk '{print $1}') == "$expected_hash" ]] || {
	echo "host release archive identity mismatch" >&2
	exit 1
}
expected_identity=$(printf '%s\n' \
	"version=$expected_version" \
	"tag=$expected_tag" \
	"commit=$expected_revision" \
	"archive-sha256=$expected_hash" \
	"patch-level=$expected_patch")
[[ -f $identity && ! -L $identity && $(cat "$identity") == "$expected_identity" ]] || {
	echo "host extracted-source identity mismatch" >&2
	exit 1
}
make -s -C "$repo" noct-host-source-verify
[[ -f $repo/build/host-noct-state/built-$expected_version-$expected_patch-process ]] || {
	echo "external host Noct build stamp is incomplete" >&2
	exit 1
}

cd "$repo"
"$noct" --help >"$output/help.log" 2>&1 || :
rg -q -- '--path=DIR1:DIR2' "$output/help.log"

if "$noct" -j0 --path= "$fixture_rel/main.noct" \
	>"$output/empty-path.log" 2>&1; then
	echo "empty --path was accepted" >&2
	exit 1
fi
rg -q 'Invalid --path option' "$output/empty-path.log"

if "$noct" -j0 --path "$fixture_rel/main.noct" \
	>"$output/missing-equals.log" 2>&1; then
	echo "malformed --path without '=' was accepted" >&2
	exit 1
fi
rg -q 'Unknown option --path' "$output/missing-equals.log"

module_path=$fixture_rel/modules-first:$fixture_rel/modules-second
printf 'NOCT-P010-PATH-FIRST\n' >"$output/expected.txt"
"$noct" -j0 --path="$module_path" "$fixture_rel/main.noct" \
	>"$output/runtime.out"
cmp "$output/expected.txt" "$output/runtime.out"

if ! "$noct" --compile --app --path="$module_path" \
	"$output/path-contract.nap" "$fixture_rel/main.noct" \
	>"$output/compile.log" 2>&1; then
	echo "--compile --app --path failed" >&2
	cat "$output/compile.log" >&2
	compile_result=fail
	failures=1
else
	"$noct" -j0 "$output/path-contract.nap" >"$output/application.out"
	cmp "$output/expected.txt" "$output/application.out"
	compile_result=pass
fi

if "$noct" -j0 --path="$output/does-not-exist" "$fixture_rel/missing.noct" \
	>"$output/missing-module.log" 2>&1; then
	echo "missing --path module was accepted" >&2
	exit 1
fi
rg -q "Cannot resolve required module 'absent_module'" \
	"$output/missing-module.log"

git grep -n -F '$(NOCT)' -- Makefile 'platform/*.mk' 'platform/*/*.mk' \
	>"$output/live-noct-recipes.txt"
live_count=0
while IFS= read -r recipe; do
	case $recipe in
	*'--checker-runner $(NOCT)'*)
		continue
		;;
	*'plan/ws010-scripting/tests/toolchain-smoke.noct'*)
		continue
		;;
	esac
	if [[ $recipe == *'$(NOCT)'* ]]; then
		((live_count += 1))
		[[ $recipe == *'--path='* ]] || {
			echo "live Noct recipe lacks --path=: $recipe" >&2
			exit 1
		}
	fi
done <"$output/live-noct-recipes.txt"
((live_count > 0)) || {
	echo "no live Noct --path recipes were audited" >&2
	exit 1
}

{
	printf 'tests=NOCT-T080,NOCT-T081,NOCT-T082,NOCT-T085\n'
	printf 'version=%s\n' "$expected_version"
	printf 'tag_commit=%s\n' "$expected_revision"
	printf 'archive_sha256=%s\n' "$expected_hash"
	printf 'noct=%s\n' "$noct"
	printf 'runtime_path=pass\n'
	printf 'compile_application_path=%s\n' "$compile_result"
	printf 'malformed_and_missing_path=pass\n'
	printf 'live_path_recipe_count=%d\n' "$live_count"
} >"$output/metadata.txt"
if ((failures != 0)); then
	printf 'WS008 host Noct --path contract: FAIL (%s)\n' "$output" >&2
	exit 1
fi
printf 'WS008 host Noct --path contract: PASS (%s; %d live recipes)\n' \
	"$output" "$live_count"
