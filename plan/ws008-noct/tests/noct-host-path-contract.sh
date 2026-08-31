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
for command in awk cmp git rg; do
	command -v "$command" >/dev/null || {
		echo "required command not found: $command" >&2
		exit 2
	}
done
[[ -x $noct && -d $source_dir/.git ]] || {
	echo "pinned host Noct is not built: $noct" >&2
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

expected_revision=$(awk '
	$1 == "ZEDBSD_HOST_NOCT_REVISION" && $2 == "?=" { print $3 }
' "$repo/Makefile")
actual_revision=$(git -C "$source_dir" rev-parse HEAD)
[[ $expected_revision =~ ^[0-9a-f]{40}$ &&
   $actual_revision == "$expected_revision" ]] || {
	echo "host checkout does not match the full zedBSD pin" >&2
	echo "expected: ${expected_revision:-<missing>}" >&2
	echo "actual:   $actual_revision" >&2
	exit 1
}
[[ -z $(git -C "$source_dir" symbolic-ref -q HEAD || true) ]] || {
	echo "host checkout is not detached at its pinned revision" >&2
	exit 1
}
checkout_status=$(git -C "$source_dir" status \
	--porcelain=v1 --untracked-files=all)
[[ -z $checkout_status ]] || {
	echo "host checkout is not clean" >&2
	printf '%s\n' "$checkout_status" >&2
	exit 1
}
if find "$source_dir" -maxdepth 1 -name '.zedbsd-checkout-*' -print -quit |
	rg -q . ||
   find "$source_dir/build-static" -maxdepth 1 -name '.zedbsd-built-*' \
	-print -quit | rg -q .; then
	echo "legacy zedBSD state stamp remains inside the upstream checkout" >&2
	exit 1
fi
[[ -f $repo/build/host-noct-state/checkout-$expected_revision &&
   -f $repo/build/host-noct-state/built-$expected_revision-process ]] || {
	echo "external host Noct state stamps are incomplete" >&2
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
	printf 'revision=%s\n' "$actual_revision"
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
