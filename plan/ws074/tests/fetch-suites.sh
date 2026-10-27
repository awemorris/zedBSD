#!/bin/sh
# ws074: fetches the conformance suites browser is measured with, at pinned commits,
# into build/ws074-suites/NAME, and checks each one's commit and licence.
#
#   sh plan/ws074/tests/fetch-suites.sh NAME...     (html5lib, wpt, test262, wasm-spec)
#
# The suites are never copied into the source tree (design.md §14.2).  Each is a shallow git
# fetch of one commit; WPT and test262 are sparse (only the directories the runners read).
# A suite already present at the pinned commit is left alone.  Changing a pin changes the
# denominators in plan/ws074/results/, which are re-based when it happens.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
root=$(pwd)/build/ws074-suites
mkdir -p "$root"

# Fetches one commit of a repository, optionally sparse, and checks the commit.
# fetch_git NAME URL COMMIT LICENSE-FILE LICENSE-PHRASE [SPARSE-PATH...]
fetch_git() {
	name=$1
	url=$2
	commit=$3
	license=$4
	phrase=$5
	shift 5
	dir=$root/$name
	if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" = "$commit" ]; then
		echo "$name: present at $commit"
		# The sparse paths may have grown since the suite was fetched (the whole commit is there already).
		if [ $# -gt 0 ]; then
			git -C "$dir" sparse-checkout set --no-cone "/$license" "$@"
		else
			git -C "$dir" sparse-checkout disable
		fi
	else
		if [ -e "$dir" ]; then
			echo "$name: existing checkout differs from the pinned commit; ask the Queue manager to clean $dir" >&2
			exit 1
		fi
		mkdir -p "$dir"
		git -C "$dir" init -q
		git -C "$dir" remote add origin "$url"
		if [ $# -gt 0 ]; then
			git -C "$dir" sparse-checkout set --no-cone "/$license" "$@"
		fi
		git -C "$dir" fetch -q --depth 1 origin "$commit"
		git -C "$dir" checkout -q FETCH_HEAD
	fi
	actual=$(git -C "$dir" rev-parse HEAD)
	if [ "$actual" != "$commit" ]; then
		echo "$name: commit $actual is not the pinned $commit" >&2
		exit 1
	fi
	if ! grep -q "$phrase" "$dir/$license"; then
		echo "$name: $license does not carry the expected licence ($phrase)" >&2
		exit 1
	fi
	echo "$name: $commit, licence checked ($license: $phrase)"
}

# Fetches one file by URL and checks its SHA-256.
# fetch_file NAME URL SHA256
fetch_file() {
	name=$1
	url=$2
	sum=$3
	dir=$root/$name
	mkdir -p "$dir"
	file=$dir/$(basename -- "$url")
	if [ ! -f "$file" ] || [ "$(sha256sum "$file" | cut -d' ' -f1)" != "$sum" ]; then
		curl --fail --location --silent --show-error -o "$file.part" "$url"
		mv "$file.part" "$file"
	fi
	actual=$(sha256sum "$file" | cut -d' ' -f1)
	if [ "$actual" != "$sum" ]; then
		echo "$name: $file has SHA-256 $actual, not the pinned $sum" >&2
		exit 1
	fi
	echo "$name: $file, SHA-256 checked"
}

for suite in "$@"; do
	case $suite in
	html5lib)
		# MIT licence.
		fetch_git html5lib https://github.com/html5lib/html5lib-tests \
			224991ec10db04f056a89eed8b0bd8695fd2950e LICENSE "Permission is hereby granted"
		;;
	wpt)
		# The 3-Clause BSD licence (LICENSE.md).  The whole tree: the fixed Interop 2025 selection
		# spans html/, event-timing/, pointerevents/, navigation-api/ and others, and its tests
		# load support files from across the tree.
		fetch_git wpt https://github.com/web-platform-tests/wpt \
			2d66b9b7998bb58c336138c178323ddee857b586 LICENSE.md "BSD"
		;;
	test262)
		# The 3-Clause BSD licence (Ecma International).
		fetch_git test262 https://github.com/tc39/test262 \
			7ab7fafa0003f73fc85c1b95d88094d33f7eb8bd LICENSE "Redistribution and use" \
			/test/ /harness/
		;;
	wasm-spec)
		# The test suite is under the Apache licence 2.0 (test/LICENSE is absent; the repository's).
		fetch_git wasm-spec https://github.com/WebAssembly/spec \
			608711107b7f1edb13efd57b7d79b49477462d36 LICENSE "Apache" \
			/test/core/
		;;
	*)
		echo "fetch-suites: unknown suite $suite" >&2
		exit 1
		;;
	esac
done
